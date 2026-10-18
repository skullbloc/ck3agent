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

// ─── Offset Header Revision ──────────────────────────────────────────────
// Bumped on every significant update to ck3_offsets.h or signatures.h.
// Lets the agent and bridge know which "generation" of offsets they have.

constexpr int OFFSET_REVISION = 1;
constexpr const char* OFFSET_DATE = "2026-04-04";

} // namespace ck3_version
