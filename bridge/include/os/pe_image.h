#pragma once
// os/pe_image.h — a bounds-checked PE32+ header reader in standard C++17.
//
// The Windows cells read the loaded ck3.exe through it (image_sections,
// binary_identity); test_os_pe_image drives it over in-memory fixtures on all
// three OSes, so the code that decides the image's identity runs on every
// push even where no Windows runner looks. No <windows.h>: each field offset
// is written out from the PE/COFF specification next to its name.
//
// A PE on disk (Layout::file) and the same PE mapped by the loader
// (Layout::loaded) share headers but not section-data placement. The debug
// directory carries both an RVA (AddressOfRawData, valid when loaded) and a
// file offset (PointerToRawData); the reader uses the one matching the layout
// it is told, and never guesses.

#include <cstddef>
#include <cstdint>

namespace bridge {
namespace os {
namespace pe {

enum class Layout : int { file = 0, loaded };

enum class ParseError : int {
    none = 0,
    too_small,            // the view is shorter than a header being read
    bad_dos_magic,        // no "MZ"
    bad_pe_offset,        // e_lfanew points outside the view
    bad_pe_signature,     // no "PE\0\0"
    not_pe32plus,         // optional-header magic is not 0x20b
    bad_optional_header,  // SizeOfOptionalHeader too small for the fields read
    bad_section_table,    // the section table runs past the view
    bad_debug_directory,  // the debug directory or a CodeView record runs past the view
};
const char* parse_error_string(ParseError e);  // the enumerator's name

struct Headers {
    std::uint16_t machine;             // FileHeader.Machine (0x8664 AMD64)
    std::uint16_t section_count;       // FileHeader.NumberOfSections
    std::uint32_t timestamp;           // FileHeader.TimeDateStamp
    std::uint64_t image_base;          // OptionalHeader.ImageBase (preferred base)
    std::uint32_t size_of_image;       // OptionalHeader.SizeOfImage
    std::uint32_t size_of_headers;     // OptionalHeader.SizeOfHeaders
    std::uint32_t debug_dir_rva;       // DataDirectory[6].VirtualAddress (0 = none)
    std::uint32_t debug_dir_size;      // DataDirectory[6].Size
    std::size_t   section_table_offset;  // byte offset of the first IMAGE_SECTION_HEADER
};

struct SectionHeader {
    char          name[9];             // NUL-terminated (8 bytes max in the image)
    std::uint32_t virtual_address;     // RVA
    std::uint32_t virtual_size;
    std::uint32_t raw_offset;          // PointerToRawData
    std::uint32_t raw_size;            // SizeOfRawData
    std::uint32_t characteristics;
};

ParseError parse_headers(const std::uint8_t* view, std::size_t size, Headers* out);
// i < h.section_count; the table was bounds-checked by parse_headers.
SectionHeader section(const std::uint8_t* view, const Headers& h, std::size_t i);
// IMAGE_SCN_MEM_{READ,WRITE,EXECUTE} -> os::SectionFlags bits.
std::uint32_t section_flags(std::uint32_t characteristics);

// RVA -> byte offset in a file-layout view (via the section table); false
// when no section covers it.
bool rva_to_file_offset(const std::uint8_t* view, const Headers& h, std::uint32_t rva,
                        std::size_t* off);

struct CodeView {
    bool          present;   // an IMAGE_DEBUG_TYPE_CODEVIEW entry with an RSDS record
    std::uint8_t  guid[16];  // as stored (GUID little-endian fields)
    std::uint32_t age;
};
// Scans the debug directory for the first CodeView RSDS record. none with
// present == false when the image has no debug directory, or none of its
// entries is a readable RSDS record (e.g. a loaded image whose CodeView data
// is not mapped: AddressOfRawData == 0).
//
// `readable` (may be null: the whole view is readable) is asked before every
// read past the headers. A loaded image is reserved for SizeOfImage bytes but
// not every page in it need be readable, so the Windows cell passes a
// VirtualQuery check; a range it rejects is bad_debug_directory.
using ReadableFn = bool (*)(const std::uint8_t* p, std::size_t n);
ParseError find_codeview(const std::uint8_t* view, std::size_t size, const Headers& h,
                         Layout layout, CodeView* out, ReadableFn readable = nullptr);
// The os::BinaryIdentity spelling (os.h): canonical GUID digits, lowercase, no
// dashes, then the age as 8 hex digits. out holds 41 bytes.
void codeview_hex(const CodeView& cv, char out[41]);

}  // namespace pe
}  // namespace os
}  // namespace bridge
