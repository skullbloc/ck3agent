// action_args.h — resolve an inner action's arguments, top-level OR under "params".
//
// WHY THIS EXISTS (ck-6kvq.15):
//
// The bridge accepts an action two ways. A raw-socket caller sends its
// arguments FLAT:
//     {"action":"check","tickets":["tk_00000005"],"request_id":"..."}
// An envelope-framed caller — which is every real client: the MCP server, the
// python bridge_client, the scenario harness, bridge_test_suite.py — nests them:
//     {"action":"check","params":{"tickets":["tk_00000005"]},"request_id":"..."}
//
// The scalar json_get_* helpers (json_helpers.cpp) already resolve both. A
// hand-rolled rapidjson read does NOT: it sees only the object it is pointed
// at. So a handler that reaches for inner["tickets"] finds nothing when a real
// client calls it, and — if it was not written to tell "absent" from "empty" —
// answers success with an empty payload.
//
// That is exactly how ck-6kvq.15 shipped: `check` returned
// {"success":true,"results":[]} for every ticket in every state, making the
// async API's whole retrieval path non-functional while looking healthy. It is
// the same shape that bit `target_ids` (query_handlers.cpp, "N4 fix",
// surface-audit 2026-08-01), which was fixed in place and so taught nobody.
//
// This header is the shared fix site, deliberately engine-free so it can be
// unit-tested on a host against real client framing.
#pragma once

#include "rapidjson/document.h"

namespace bridge {

// Return the object that actually carries `key` for this action: `inner`
// itself when the caller passed arguments flat, or inner["params"] when the
// caller framed them. Falls back to `inner` when the key is nowhere, so the
// caller's own "is it present?" test stays the single source of truth on
// absence.
//
// Resolution is PER KEY on purpose. A client may legitimately mix the two —
// request_id flat, arguments framed — and some do.
const rapidjson::Value& action_args(const rapidjson::Value& inner, const char* key);

// True when `key` is present in either position. Use this instead of
// inner.HasMember(key) in any hand-rolled read.
bool action_has(const rapidjson::Value& inner, const char* key);

}  // namespace bridge
