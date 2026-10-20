// windows/identity.cpp — os::binary_identity on Windows.
//
// The key, in order (the brief, and the ADR's binary-identity row and
// amendments):
//   1. the CodeView RSDS record (PDB GUID + age) in the LOADED image's debug
//      directory, every read gated on VirtualQuery;
//   2. the same record in the image FILE on disk (file layout), when the
//      loaded copy has no mapped CodeView data (AddressOfRawData == 0);
//   3. otherwise the file's SHA-256 is the key (format image_sha256).
// SizeOfImage is always reported (image_size), and the file SHA-256 always
// rides along as the second witness (image_sha256).
//
// `check` is the P4 identity gate's verdict (identity_gate.h) against the
// pinned 1.18.3.1 constants (pinned_identity.h): "match", "mismatch", or
// "not_found" when nothing could be observed. The DLL's init thread logs the
// same outcome; neither reader re-derives it.
// Computed once per process and cached: the hash reads the whole exe, so
// callers keep the first call off the init path, as the ADR requires.

#include "win_internal.h"

#include "identity_gate.h"
#include "os/sha256.h"

#include <cstring>
#include <mutex>

namespace bridge {
namespace os {

namespace {

struct Cached {
    Result         result;
    BinaryIdentity id;
};

// Map the file read-only; hash it, and look for CodeView in file layout.
bool hash_file(const wchar_t* path, char sha_hex[65], pe::CodeView* cv, DWORD* err) {
    std::memset(cv, 0, sizeof(*cv));
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) {
        *err = GetLastError();
        CloseHandle(f);
        return false;
    }
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(f);
    if (!m) { *err = GetLastError(); return false; }
    const auto* view = static_cast<const std::uint8_t*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(m);
    if (!view) { *err = GetLastError(); return false; }
    const std::size_t size = std::size_t(sz.QuadPart);
    sha256_hex(view, size, sha_hex);
    pe::Headers h;
    if (pe::parse_headers(view, size, &h) == pe::ParseError::none)
        pe::find_codeview(view, size, h, pe::Layout::file, cv);
    UnmapViewOfFile(view);
    return true;
}

Cached compute() {
    Cached c;
    BinaryIdentity& id = c.id;
    std::memset(&id, 0, sizeof(id));
    id.format = IdentityFormat::none;
    id.check = "unchecked";
    c.result = win::ok();

    const std::uint8_t* v = nullptr;
    std::size_t n = 0;
    pe::Headers h;
    Result why = win::ok();
    if (!win::main_headers(&v, &n, &h, &why)) { c.result = why; return c; }
    id.image_size = h.size_of_image;

    pe::CodeView loaded;
    const pe::ParseError le =
        pe::find_codeview(v, h.size_of_image, h, pe::Layout::loaded, &loaded, &win::readable);

    static wchar_t path[32768];
    pe::CodeView from_file;
    std::memset(&from_file, 0, sizeof(from_file));
    DWORD err = 0;
    const bool have_sha = win::module_path_w(nullptr, path, sizeof(path) / sizeof(path[0])) &&
                          hash_file(path, id.image_sha256, &from_file, &err);
    if (!have_sha) {
        id.image_sha256[0] = '\0';
        if (!err) err = GetLastError();
    }

    if (le == pe::ParseError::none && loaded.present) {
        id.format = IdentityFormat::pe_codeview_rsds;
        pe::codeview_hex(loaded, id.observed_hex);
    } else if (from_file.present) {
        id.format = IdentityFormat::pe_codeview_rsds;
        pe::codeview_hex(from_file, id.observed_hex);
    } else if (have_sha) {
        id.format = IdentityFormat::image_sha256;
        std::memcpy(id.observed_hex, id.image_sha256, sizeof(id.observed_hex));
    } else {
        c.result = win::failed(int(err), "no CodeView record and the image file could not be hashed");
        return c;
    }
    if (!have_sha)
        c.result = Result{Status::ok, int(err), "the image file could not be hashed; image_sha256 is empty"};
    return c;
}

}  // namespace

Result binary_identity(BinaryIdentity* out) {
    if (!out) return win::failed(ERROR_INVALID_PARAMETER, "out is null");
    static std::once_flag once;
    static Cached cached;
    std::call_once(once, [] {
        cached = compute();
        cached.id.check = identity_gate(cached.id, pinned_identity()).check;
    });
    *out = cached.id;
    return cached.result;
}

}  // namespace os
}  // namespace bridge
