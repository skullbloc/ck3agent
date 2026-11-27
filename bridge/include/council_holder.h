// include/council_holder.h — who sits in a council seat, ONE definition for
// query_council and appoint_councillor.
//
// A council position instance carries two holder fields (re/offsets/ck3_offsets.h):
//   +0x30 COUNCIL_POS_HOLDER_ID      the DISPLAY holder: reads 0xFFFFFF even for FILLED seats (always for the
//                                    chaplain / consort seats, and on some AI realms on EVERY seat, e.g. Murchad's:
//                                    query_council said holder_source "seated" for each);
//   +0x38 COUNCIL_POS_COUNCILLOR_ID  the AUTHORITATIVE seated councillor (do_set_council_task's ctx_a).
// query_council was fixed (20260619) to prefer +0x30 when populated and else take +0x38; appoint_councillor
// read +0x30 alone, so on such a realm every seat read vacant and appoint answered
// vacant_seat_fill_unsupported for a FILLED seat (the Francia run's "vacant steward seat"), and its readback
// after the swap read no new holder.
#pragma once
#include <cstdint>

namespace council_holder {

inline bool is_id(uint32_t v) { return v != 0 && v != 0xFFFFFFu; }

enum class Source { None, Display, Seated };

// The seat's holder from the two raw u32 fields (masked here to 24 bits): the display field when it names
// someone, else the seated councillor; 0xFFFFFF when neither does. `src` (optional) says which field answered.
inline uint32_t resolve(uint32_t display_raw, uint32_t seated_raw, Source* src = nullptr) {
    const uint32_t d = display_raw & 0xFFFFFFu, s = seated_raw & 0xFFFFFFu;
    if (is_id(d)) { if (src) *src = Source::Display; return d; }
    if (is_id(s)) { if (src) *src = Source::Seated; return s; }
    if (src) *src = Source::None;
    return 0xFFFFFFu;
}

inline const char* source_name(Source s) {
    return s == Source::Display ? "display" : s == Source::Seated ? "seated" : "none";
}

}  // namespace council_holder
