// Offset Version Header — Machine-readable BuildID + game version tracking
// =========================================================================
//
// This header is THE authoritative record of which CK3 binary the offsets
// in ck3_offsets.h and signatures.h target. It is consumed by the runtime
// resolver to verify the loaded binary matches at init time.
//
// On patch-adapt, the previous version of this header (and ck3_offsets.h,
// signatures.h) should be copied to re/offsets/archive/<buildid_prefix>/
// before updating.
//
#pragma once
#include <cstdint>
#include <cstddef>

namespace ck3_version {

// ─── Target Binary Identity ──────────────────────────────────────────────
// BuildID from ELF .note.gnu.build-id section (20 bytes / 40 hex chars).
// This is the primary key — it uniquely identifies the exact binary.

constexpr uint8_t BUILD_ID[] = {
    0xe6, 0x1f, 0xf2, 0x64, 0x2f, 0x07, 0x09, 0x4d, 0xef, 0xd1,
    0x40, 0xfc, 0xc9, 0x8e, 0x07, 0x8d, 0x42, 0x54, 0x12, 0x75
};
constexpr size_t BUILD_ID_LEN = sizeof(BUILD_ID);

// Human-readable hex string for logging
constexpr const char* BUILD_ID_HEX = "e61ff2642f07094defd140fcc98e078d42541275";

// ─── Game Version (informational) ────────────────────────────────────────
// These are best-effort from the binary's embedded version strings.
// The BuildID is the authoritative identity — version is for human context.

constexpr const char* GAME_VERSION  = "1.18.3.1"; // CK3 patch version. HIGH: the Steam install
                                                   // carrying this BuildID reports "1.18.3.1 (Crane)"
                                                   // in launcher/launcher-settings.json and embeds the
                                                   // same string; the earlier "1.12.5" label was never
                                                   // checked against the binary (release-prep 2026-10-17).
constexpr const char* GAME_PLATFORM = "linux";     // linux / windows
constexpr const char* BINARY_MD5    = "09ba163bbff47f2eb8a69692536307e6";

// ─── Windows PE identity (ADR 0001 "Binary identity"; lane-p0p1 2026-10-19) ──
// Per-platform siblings of BUILD_ID for the Windows build of the SAME pinned
// version: ck3.exe of CK3 1.18.3.1 from the Steam `1.18.3` branch (buildid
// 21768289; launcher-settings.json rawVersion "1.18.3.1"; titus_branch.txt
// release/1.18.3, titus_rev.txt 7b069e36…, equal to the Linux reference).
// Nothing in the Linux build reads them. Each value HIGH: the inspect JSON
// (tools/inspect_binary.py on the depot) and the independent parser
// re/scripts/windows/pe_manual.py agree byte for byte; SHA-256 also by
// sha256sum on node and workstation. Evidence: re/findings/windows/W0-binary-report.md.
namespace windows {
constexpr const char* PE_GAME_VERSION = "1.18.3.1";   // HIGH: launcher-settings.json rawVersion

// CodeView RSDS record (debug directory entry type 2). GUID as text, and the
// 16 raw bytes as stored in the file (Data1/2/3 little-endian, then Data4).
constexpr const char* PE_PDB_GUID_STR = "3E26A827-C47A-4FB0-A863-3288849287C8";  // HIGH
constexpr uint8_t PE_PDB_GUID[] = {
    0x27, 0xa8, 0x26, 0x3e, 0x7a, 0xc4, 0xb0, 0x4f,
    0xa8, 0x63, 0x32, 0x88, 0x84, 0x92, 0x87, 0xc8
};                                                    // HIGH
constexpr uint32_t PE_PDB_AGE       = 1;              // HIGH
// TimeDateStamp is a clock time (no REPRO debug entry; 2026-02-03T15:58:44Z,
// the `1.18.3` branch's Steam build date), so TIMESTAMP + SIZE_OF_IMAGE is a
// usable fallback key if the debug directory were ever absent.
constexpr uint32_t PE_TIMESTAMP     = 0x69821B34;     // HIGH: 1770134324
constexpr uint32_t PE_SIZE_OF_IMAGE = 0x05A6A000;     // HIGH: 94806016
constexpr uint64_t IMAGE_BASE       = 0x140000000ull; // HIGH: optional-header ImageBase (Ghidra VA = IMAGE_BASE + RVA)
// Second identity witness (ADR amendment): SHA-256 of the image file on disk.
constexpr const char* IMAGE_SHA256  =
    "2f1358c0ed3378656fa0fea7738d79d0fa30c9bb23d365d76aa647d0a2fccb0f";  // HIGH (93,388,408 bytes)
} // namespace windows

// ─── Offset Header Revision ──────────────────────────────────────────────
// Bumped on every significant update to ck3_offsets.h or signatures.h.
// Lets the agent and bridge know which "generation" of offsets they have.

constexpr int OFFSET_REVISION = 1;
constexpr const char* OFFSET_DATE = "2026-04-04";

} // namespace ck3_version
