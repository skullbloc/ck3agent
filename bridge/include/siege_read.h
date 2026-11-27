// siege_read.h — one siege's reads and their verdict, emitted the same way by every reply that
// shows a siege (query_wars sieges[], query_armies' army position); the verdict is
// siege_view.h (engine-free), the reads siege_read.cpp.
#pragma once

#include <cstdint>

#include "siege_view.h"

namespace bridge {

class ResponseBuilder;

// The men of an army now (its regiments' current men) and its best siege tier (0 = no siege weapons).
int32_t army_men_and_tier(uintptr_t army, int* siege_tier_out);

// Read the siege at `siege` (a live CSiege*). False when its province is not readable.
bool read_siege(uintptr_t siege, siege_view::Reads* out);

// Emit the siege's fields into the open object: progress, progress_needed_max, progress_pct, status,
// blocker, fort_level, garrison, men, siege_tier, tier_needed, fort_speed, walls, next_event_days,
// eta_days, assault. Nothing when the siege could not be read (then "progress_unread": true).
void emit_siege_view(ResponseBuilder& r, uintptr_t siege);

}  // namespace bridge
