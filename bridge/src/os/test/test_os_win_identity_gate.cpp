// test_os_win_identity_gate.cpp — the P4 identity gate (windows/identity_gate.h)
// on synthetic identities, engine-free, under wine. The gate is pure, so every
// branch is reachable without the game: match on the pin, mismatch on the
// other build (1.20.1.1's CodeView GUID), on age alone, on SizeOfImage alone,
// the SHA-256 fallback key, and nothing observed.
//
// The pinned CodeView spelling is re-derived here from the dashed GUID the
// inspect JSON prints, through pe::codeview_hex over the bytes as a PE stores
// them (little-endian Data1..3), so a typo in pinned_identity.h's hex cannot
// agree with itself.

#include "os/os.h"
#include "os/pe_image.h"

#include "../windows/identity_gate.h"
#include "../windows/pinned_identity.h"

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

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "3E26A827-C47A-4FB0-A863-3288849287C8", age -> the codeview_hex spelling.
static std::string cv_from_dashed(const char* dashed, std::uint32_t age) {
    std::uint8_t canon[16];
    int k = 0;
    for (const char* p = dashed; *p && k < 32; ++p) {
        if (*p == '-') continue;
        const int v = hexval(*p);
        if (v < 0) return "";
        if (k % 2 == 0) canon[k / 2] = std::uint8_t(v << 4);
        else canon[k / 2] = std::uint8_t(canon[k / 2] | v);
        ++k;
    }
    if (k != 32) return "";
    os::pe::CodeView cv;
    std::memset(&cv, 0, sizeof(cv));
    cv.present = true;
    // As stored in the RSDS record: Data1 (u32 LE), Data2, Data3 (u16 LE), Data4 bytes.
    const int stored[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) cv.guid[i] = canon[stored[i]];
    cv.age = age;
    char out[41];
    os::pe::codeview_hex(cv, out);
    return out;
}

static os::BinaryIdentity ident(os::IdentityFormat f, const std::string& key, std::uint64_t size,
                                const char* sha) {
    os::BinaryIdentity b;
    std::memset(&b, 0, sizeof(b));
    b.format = f;
    b.check = "unchecked";
    std::strncpy(b.observed_hex, key.c_str(), sizeof(b.observed_hex) - 1);
    std::strncpy(b.image_sha256, sha, sizeof(b.image_sha256) - 1);
    b.image_size = size;
    return b;
}

static bool is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

int main() {
    const os::PinnedIdentity& pin = os::pinned_identity();
    const std::string pinned_cv = cv_from_dashed("3E26A827-C47A-4FB0-A863-3288849287C8", 1);
    const std::string other_cv = cv_from_dashed("0DF59548-663D-4C98-A61C-878362242E2B", 1);
    const char* sha = "2f1358c0ed3378656fa0fea7738d79d0fa30c9bb23d365d76aa647d0a2fccb0f";
    const char* other_sha = "0000000000000000000000000000000000000000000000000000000000000001";

    check(pinned_cv.size() == 40 && pinned_cv == pin.codeview_hex,
          "pin: kCodeViewHex == codeview_hex(GUID 3E26A827-..., age 1) re-derived from the dashed form");
    check(pin.size_of_image == 94806016u, "pin: SizeOfImage 94806016");
    check(is(pin.image_sha256, sha), "pin: sha256 2f1358c0...");
    check(os::pinned::kImageBase == 0x140000000ull, "pin: ImageBase 0x140000000 (context)");

    using F = os::IdentityFormat;
    os::GateOutcome g;

    g = os::identity_gate(ident(F::pe_codeview_rsds, pinned_cv, 94806016u, sha), pin);
    check(is(g.check, "match") && is(g.key, "codeview+size_of_image") && is(g.sha_witness, "agreed") &&
              is(g.why, ""),
          "pinned CodeView + SizeOfImage + sha: match, sha witness agreed");

    g = os::identity_gate(ident(F::pe_codeview_rsds, pinned_cv, 94806016u, ""), pin);
    check(is(g.check, "match") && is(g.sha_witness, "witness_absent"),
          "no file hash: still match on key 1, and the witness is absent, not agreed");

    g = os::identity_gate(ident(F::pe_codeview_rsds, pinned_cv, 94806016u, other_sha), pin);
    check(is(g.check, "match") && is(g.sha_witness, "disagreed"),
          "sha differs under key 1: match on the key, witness disagreed (reported, not gated)");

    g = os::identity_gate(ident(F::pe_codeview_rsds, other_cv, 94806016u, other_sha), pin);
    check(is(g.check, "mismatch") && std::strstr(g.why, "GUID") != nullptr,
          "1.20.1.1's GUID: mismatch, why names the GUID");

    g = os::identity_gate(ident(F::pe_codeview_rsds, cv_from_dashed("3E26A827-C47A-4FB0-A863-3288849287C8", 2),
                                94806016u, sha), pin);
    check(is(g.check, "mismatch"), "same GUID, age 2: mismatch");

    g = os::identity_gate(ident(F::pe_codeview_rsds, pinned_cv, 94806016u + 4096u, sha), pin);
    check(is(g.check, "mismatch") && std::strstr(g.why, "SizeOfImage") != nullptr &&
              std::strstr(g.why, "GUID") == nullptr,
          "SizeOfImage alone differs: mismatch, why names SizeOfImage only");

    g = os::identity_gate(ident(F::pe_codeview_rsds, other_cv, 1, sha), pin);
    check(is(g.check, "mismatch") && std::strstr(g.why, "both") != nullptr, "both differ: why says both");

    g = os::identity_gate(ident(F::image_sha256, sha, 94806016u, sha), pin);
    check(is(g.check, "match") && is(g.key, "image_sha256"), "no CodeView, pinned sha + size: match on key 2");

    g = os::identity_gate(ident(F::image_sha256, other_sha, 94806016u, other_sha), pin);
    check(is(g.check, "mismatch") && is(g.key, "image_sha256"), "no CodeView, other sha: mismatch");

    g = os::identity_gate(ident(F::image_sha256, sha, 1, sha), pin);
    check(is(g.check, "mismatch"), "no CodeView, pinned sha but other SizeOfImage: mismatch");

    g = os::identity_gate(ident(F::none, "", 0, ""), pin);
    check(is(g.check, "not_found") && is(g.key, "none") && is(g.sha_witness, "witness_absent"),
          "nothing observed: not_found (refuses), never agreed");

    // An ELF identity reaching the PE gate is not a match either.
    g = os::identity_gate(ident(F::elf_gnu_build_id, pinned_cv, 94806016u, sha), pin);
    check(!is(g.check, "match"), "an ELF-format identity never matches the PE pin");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
