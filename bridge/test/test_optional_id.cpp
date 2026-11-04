// test_optional_id.cpp — witness r1 item 8: the location_id fix
// (action_handlers.cpp resolve_build_target) through the REAL json_get_int.
// Built twice: plain (must pass) and -DOPTIONAL_ID_PREFIX_CONTROL, the pre-fix
// read, registered WILL_FAIL (an absent location_id reads as 0xFFFFFF, so
// missing_location_id never fires).
#include <cstdio>
#include <cstdint>
#include "optional_id.h"

namespace bridge { int json_get_int(const char* json, const char* key); }

static int failures = 0;
static void expect(const char* params, uint32_t want) {
    const uint32_t got = bridge::optional_id(bridge::json_get_int(params, "location_id"));
    const bool ok = got == want;
    printf("%s %-52s -> 0x%x\n", ok ? "PASS" : "FAIL", params, got);
    if (!ok) failures++;
}

int main() {
    expect("{\"building\":\"castle_01\"}", 0);                         // absent: missing_location_id
    expect("{\"params\":{\"building\":\"castle_01\"}}", 0);            // absent, params-wrapped
    expect("{\"location_id\":-1}", 0);                                 // the engine's none-ref
    expect("{\"location_id\":0}", 0);
    expect("{\"location_id\":4242}", 4242);
    expect("{\"location_id\":184553618}", 4242);                       // 0x0B001092: generation byte dropped
    expect("{\"params\":{\"location_id\":4242}}", 4242);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
