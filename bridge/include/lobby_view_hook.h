// lobby_view_hook.h — ck-dq7ym.8
//
// Captures the live CJominiMultiplayerLobbyView singleton via an inline
// constructor hook. See bridge/src/lobby_view_hook.cpp for the rationale.

#pragma once

namespace ck3_bridge {

// Install the constructor hook. Idempotent at the bridge layer — called once
// from bridge_init. Returns true on success; on failure, the rest of the
// bridge remains functional but observer-mode programmatic entry is unavailable.
bool install_lobby_view_hook();

// Live singleton pointer. nullptr until the engine constructs the lobby view
// (after a save loads with multiplayer/observer flags set). Subsequent
// destruction does NOT clear this — a follow-up bead can add a destructor
// hook if invalidation is needed.
void* get_lobby_view_singleton();

}  // namespace ck3_bridge
