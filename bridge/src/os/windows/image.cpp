// windows/image.cpp — os::image_base and os::image_sections on Windows.
//
// The load base is GetModuleHandleW(NULL) (ADR amendment "PE address
// convention": an offset under re/offsets/windows/ is an RVA, address minus
// this base). The extent and the section table come from the loaded image's
// own headers through the portable reader (os/pe_image.h), bounded by the
// header region VirtualQuery reports, so a corrupt header cannot walk the
// reader off the map.

#include "win_internal.h"

#include <cstring>

namespace bridge {
namespace os {

Result image_base(ImageInfo* out) {
    if (!out) return win::failed(ERROR_INVALID_PARAMETER, "out is null");
    out->base = 0;
    out->end = 0;
    const std::uint8_t* v = nullptr;
    std::size_t n = 0;
    pe::Headers h;
    Result why = win::ok();
    if (!win::main_headers(&v, &n, &h, &why)) return why;
    out->base = reinterpret_cast<std::uintptr_t>(v);
    out->end = out->base + h.size_of_image;
    return win::ok();
}

Result image_sections(Section* out, std::size_t cap, std::size_t* count) {
    if (!count) return win::failed(ERROR_INVALID_PARAMETER, "count is null");
    *count = 0;
    if (cap && !out) return win::failed(ERROR_INVALID_PARAMETER, "out is null with cap > 0");
    const std::uint8_t* v = nullptr;
    std::size_t n = 0;
    pe::Headers h;
    Result why = win::ok();
    if (!win::main_headers(&v, &n, &h, &why)) return why;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(v);
    *count = h.section_count;
    for (std::size_t i = 0; i < h.section_count && i < cap; ++i) {
        const pe::SectionHeader s = pe::section(v, h, i);
        std::memcpy(out[i].name, s.name, sizeof(out[i].name));
        out[i].start = base + s.virtual_address;
        out[i].end = out[i].start + s.virtual_size;
        out[i].flags = pe::section_flags(s.characteristics);
    }
    if (h.section_count > cap)
        return win::failed(ERROR_INSUFFICIENT_BUFFER, "more sections than cap; *count says how many");
    return win::ok();
}

}  // namespace os
}  // namespace bridge
