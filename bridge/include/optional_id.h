// optional_id.h — an optional id argument read through json_get_int, in ONE place.
//
// json_get_int answers -1 for an ABSENT key (bridge/test/json_int_absence_probe.cpp
// pins that). construct_building / construction_check read `location_id` as
//
//     t.barony_id = static_cast<uint32_t>(json_get_int(params, "location_id")) & 0xFFFFFF;
//
// so a call without it carried the id 0xFFFFFF and missing_location_id never
// fired (L20-A, the A3 class; witness r1 item 8: the fix had no test). The rule:
// absent, negative or 0 -> 0 (the caller's "missing" refusal), anything else ->
// its low 24 bits (a full handle's generation byte is ignored).
// Header-only and engine-free: bridge/test/test_optional_id.cpp compiles it.
#ifndef CK3_BRIDGE_OPTIONAL_ID_H
#define CK3_BRIDGE_OPTIONAL_ID_H

#include <cstdint>

namespace bridge {

// `raw` is json_get_int(params, key) as the handler received it.
inline uint32_t optional_id(int raw) {
#ifdef OPTIONAL_ID_PREFIX_CONTROL
    return static_cast<uint32_t>(raw) & 0xFFFFFFu;  // the pre-fix read (ctest control)
#else
    return raw > 0 ? static_cast<uint32_t>(raw) & 0xFFFFFFu : 0u;
#endif
}

}  // namespace bridge

#endif  // CK3_BRIDGE_OPTIONAL_ID_H
