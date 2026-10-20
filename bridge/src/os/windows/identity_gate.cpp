// windows/identity_gate.cpp — the P4 identity gate (identity_gate.h).

#include "identity_gate.h"

#include "pinned_identity.h"

#include <cstring>

namespace bridge {
namespace os {

namespace {

bool eq(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

const char* sha_witness(const BinaryIdentity& id, const PinnedIdentity& pin) {
    if (id.image_sha256[0] == '\0') return "witness_absent";
    return eq(id.image_sha256, pin.image_sha256) ? "agreed" : "disagreed";
}

}  // namespace

GateOutcome identity_gate(const BinaryIdentity& id, const PinnedIdentity& pin) {
    GateOutcome g{"not_found", "none", "", sha_witness(id, pin)};
    switch (id.format) {
    case IdentityFormat::pe_codeview_rsds: {
        g.key = "codeview+size_of_image";
        const bool cv = eq(id.observed_hex, pin.codeview_hex);
        const bool sz = id.image_size == pin.size_of_image;
        g.check = (cv && sz) ? "match" : "mismatch";
        g.why = (cv && sz) ? ""
                : (!cv && !sz) ? "CodeView GUID+age and SizeOfImage both differ from the pin"
                : !cv          ? "CodeView GUID+age differs from the pin"
                               : "SizeOfImage differs from the pin (CodeView agrees)";
        return g;
    }
    case IdentityFormat::image_sha256: {
        g.key = "image_sha256";
        const bool sh = eq(id.image_sha256, pin.image_sha256);
        const bool sz = id.image_size == pin.size_of_image;
        g.check = (sh && sz) ? "match" : "mismatch";
        g.why = (sh && sz) ? ""
                : !sh      ? "no CodeView record; the image file SHA-256 differs from the pin"
                           : "no CodeView record; SizeOfImage differs from the pin (SHA-256 agrees)";
        return g;
    }
    default:
        g.why = "no identity was observed (no CodeView record and no file hash)";
        return g;
    }
}

const PinnedIdentity& pinned_identity() {
    static const PinnedIdentity p{pinned::kCodeViewHex, pinned::kSizeOfImage, pinned::kImageSha256};
    return p;
}

}  // namespace os
}  // namespace bridge
