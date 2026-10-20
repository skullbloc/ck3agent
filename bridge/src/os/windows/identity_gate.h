#pragma once
// windows/identity_gate.h — the P4 identity gate: is the host ck3.exe the one
// build the bridge's offsets fit? (ADR 0001 phase P4.) Internal to
// bridge/src/os/windows/.
//
// One computation, two readers: identity.cpp stamps its verdict into
// os::BinaryIdentity::check (what a later handshake reports), and the DLL's
// init thread (dll_main.cpp) logs the same outcome and decides whether to
// start anything. Neither re-derives it.
//
// The key, in order (the brief and the ADR's binary-identity row):
//   1. format pe_codeview_rsds: the CodeView GUID+age AND SizeOfImage must
//      both equal the pin. Either one different is a mismatch, and `why`
//      names which.
//   2. format image_sha256 (no CodeView record could be read): the file
//      SHA-256 must equal the pinned one (and SizeOfImage too).
//   3. format none / nothing observed: not_found, which REFUSES like a
//      mismatch: "could not see" is never "agreed".
// The file SHA-256 is not gated on under key 1, but it is reported beside the
// verdict as a second witness (sha_witness): agreed / disagreed /
// witness_absent. A witness that was not taken is a state, not a default.

#include "os/os.h"

#include <cstdint>

namespace bridge {
namespace os {

struct PinnedIdentity {
    const char*   codeview_hex;   // pe::codeview_hex spelling, 40 hex digits
    std::uint64_t size_of_image;
    const char*   image_sha256;   // 64 lowercase hex digits
};

struct GateOutcome {
    const char* check;        // "match" | "mismatch" | "not_found" (os.h vocabulary)
    const char* key;          // "codeview+size_of_image" | "image_sha256" | "none"
    const char* why;          // static; what differed, or "" on match
    const char* sha_witness;  // "agreed" | "disagreed" | "witness_absent"
};

// Pure: reads only its arguments.
GateOutcome identity_gate(const BinaryIdentity& observed, const PinnedIdentity& pin);

// The pin this build carries (windows/pinned_identity.h).
const PinnedIdentity& pinned_identity();

}  // namespace os
}  // namespace bridge
