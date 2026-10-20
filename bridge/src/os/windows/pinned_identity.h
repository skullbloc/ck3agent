#pragma once
// windows/pinned_identity.h — the identity of the ONE Windows ck3.exe the
// bridge accepts (ADR 0001 phase P4: `match` on 1.18.3.1, REFUSED on any other
// build). Plain constants, no code.
//
// EVENTUAL SOURCE: re/offsets/offset_version.h, Windows section (PE_PDB_GUID,
// PE_PDB_AGE, PE_SIZE_OF_IMAGE, ...; the ADR's "Binary identity on the Python
// side"). Another lane (lane-p0p1) owns that header; until its Windows
// constants land, the gate reads them from here so the two edits do not
// conflict. When they land, this header becomes aliases of those constants
// (or is deleted): never a second copy that can drift.
//
// Confidence: HIGH. Source: tools/inspect_binary.py --json over the pinned
// Windows depot's binaries/ck3.exe (Steam branch `1.18.3`, launcher
// rawVersion 1.18.3.1, titus_branch.txt release/1.18.3, titus_rev 7b069e36),
// read 2026-10-18 from the pinned 1.18.3 depot; the sha256 was
// re-taken there with sha256sum and the CodeView/SizeOfImage pair was observed
// by this gate inside the running game under Proton (lane-p4, evidence in
// re/findings/windows/re_p4_proxy_gate.md). The record's PDB path is
// C:\mnt\gsg\ck3\titus\outputdir_windows\binaries\ck3.pdb.
//
// Spelling: kCodeViewHex is the os::BinaryIdentity spelling of a CodeView
// record (pe::codeview_hex): canonical GUID digits, lowercase, no dashes, then
// the age as 8 hex digits.

#include <cstdint>

namespace bridge {
namespace os {
namespace pinned {

constexpr const char* kLabel = "ck3.exe 1.18.3.1 (Windows, Steam branch 1.18.3)";

// CodeView RSDS: GUID {3E26A827-C47A-4FB0-A863-3288849287C8}, age 1.
constexpr const char* kCodeViewHex = "3e26a827c47a4fb0a8633288849287c8"
                                     "00000001";
// Optional header SizeOfImage, bytes.
constexpr std::uint64_t kSizeOfImage = 94806016;  // 0x5A6A000
// SHA-256 of the image FILE: the key when no CodeView record can be read.
constexpr const char* kImageSha256 =
    "2f1358c0ed3378656fa0fea7738d79d0fa30c9bb23d365d76aa647d0a2fccb0f";
// Context only, not gated on: optional header ImageBase and the file header
// TimeDateStamp (2026-02-03T15:58:44Z); file size 93,388,408 bytes.
constexpr std::uint64_t kImageBase = 0x140000000ull;
constexpr std::uint32_t kTimeDateStamp = 1770134324u;

}  // namespace pinned
}  // namespace os
}  // namespace bridge
