// os_common.cpp — the platform layer's vocabulary, compiled on every platform.
// Standard C++17 only: this TU is built by GCC, MSVC and Apple clang.

#include "os/os.h"

namespace bridge {
namespace os {

const char* status_string(Status s) {
    switch (s) {
        case Status::ok:                      return "ok";
        case Status::failed:                  return "failed";
        case Status::not_routed:              return "not_routed";
        case Status::not_implemented:         return "not_implemented";
        case Status::unavailable_on_platform: return "unavailable_on_platform";
    }
    return "unknown";
}

const char* platform_name(Platform p) {
    switch (p) {
        case Platform::kLinux:   return "linux";
        case Platform::kWindows: return "windows";
        case Platform::kMacos:   return "macos";
    }
    return "unknown";
}

const char* identity_format_string(IdentityFormat f) {
    switch (f) {
        case IdentityFormat::none:             return "none";
        case IdentityFormat::elf_gnu_build_id: return "elf_gnu_build_id";
        case IdentityFormat::pe_codeview_rsds: return "pe_codeview_rsds";
        case IdentityFormat::macho_lc_uuid:    return "macho_lc_uuid";
    }
    return "unknown";
}

const char* transport_kind_string(TransportKind k) {
    switch (k) {
        case TransportKind::none:         return "none";
        case TransportKind::af_unix:      return "af_unix";
        case TransportKind::tcp_loopback: return "tcp_loopback";
    }
    return "unknown";
}

}  // namespace os
}  // namespace bridge
