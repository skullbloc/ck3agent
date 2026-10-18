// json_helpers.cpp — Minimal rapidjson-backed accessors used by the dispatch
// router. Split from dispatch_router.cpp so unit tests can link them without
// pulling in the rest of the bridge's global state.

#include "action_dispatch_internal.h"
#include <cstring>
#include <cstdlib>
#include <rapidjson/document.h>

namespace bridge {

// Rotating pool: each call returns a pointer into the next slot, so a handler
// that reads several string params back-to-back gets distinct non-aliasing
// pointers. Pool size must exceed the max number of live string pointers any
// single handler retains simultaneously (observed max: ~4).
const char* json_get_string(const char* json, const char* key) {
    constexpr size_t BUF_SIZE = 256;
    constexpr size_t POOL_SIZE = 16;
    thread_local static char value_bufs[POOL_SIZE][BUF_SIZE];
    thread_local static size_t next_slot = 0;

    if (!json || !key) return nullptr;

    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseIterativeFlag>(json);
    if (doc.HasParseError() || !doc.IsObject()) return nullptr;

    auto it = doc.FindMember(key);
    if (it == doc.MemberEnd()) {
        auto params_it = doc.FindMember("params");
        if (params_it != doc.MemberEnd() && params_it->value.IsObject()) {
            it = params_it->value.FindMember(key);
            if (it == params_it->value.MemberEnd()) return nullptr;
        } else {
            return nullptr;
        }
    }
    if (!it->value.IsString()) return nullptr;

    char* buf = value_bufs[next_slot];
    next_slot = (next_slot + 1) % POOL_SIZE;

    size_t len = it->value.GetStringLength();
    if (len >= BUF_SIZE) len = BUF_SIZE - 1;
    memcpy(buf, it->value.GetString(), len);
    buf[len] = '\0';
    return buf;
}

int json_get_int(const char* json, const char* key) {
    if (!json || !key) return -1;

    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseIterativeFlag>(json);
    if (doc.HasParseError() || !doc.IsObject()) return -1;

    auto it = doc.FindMember(key);
    if (it == doc.MemberEnd()) {
        auto params_it = doc.FindMember("params");
        if (params_it != doc.MemberEnd() && params_it->value.IsObject()) {
            it = params_it->value.FindMember(key);
            if (it == params_it->value.MemberEnd()) return -1;
        } else {
            return -1;
        }
    }
    if (it->value.IsInt()) return it->value.GetInt();
    if (it->value.IsUint()) return static_cast<int>(it->value.GetUint());
    if (it->value.IsDouble()) return static_cast<int>(it->value.GetDouble());
    if (it->value.IsString()) {
        const char* s = it->value.GetString();
        char* end = nullptr;
        long val = strtol(s, &end, 10);
        if (end != s && *end == '\0') return static_cast<int>(val);
    }
    return -1;
}

bool json_get_bool(const char* json, const char* key, bool default_val) {
    if (!json || !key) return default_val;

    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseIterativeFlag>(json);
    if (doc.HasParseError() || !doc.IsObject()) return default_val;

    auto it = doc.FindMember(key);
    if (it == doc.MemberEnd()) {
        auto params_it = doc.FindMember("params");
        if (params_it != doc.MemberEnd() && params_it->value.IsObject()) {
            it = params_it->value.FindMember(key);
            if (it == params_it->value.MemberEnd()) return default_val;
        } else {
            return default_val;
        }
    }
    if (it->value.IsBool()) return it->value.GetBool();
    return default_val;
}

// Strict variant (N3, surface-audit 2026-08-01): succeeds ONLY for a genuine
// JSON boolean (top-level or under "params"). See header comment — pair with
// json_has_key to typed-reject present-but-non-boolean values instead of
// silently falling back to a default like json_get_bool does.
bool json_get_bool_strict(const char* json, const char* key, bool* out) {
    if (!json || !key || !out) return false;

    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseIterativeFlag>(json);
    if (doc.HasParseError() || !doc.IsObject()) return false;

    auto it = doc.FindMember(key);
    if (it == doc.MemberEnd()) {
        auto params_it = doc.FindMember("params");
        if (params_it == doc.MemberEnd() || !params_it->value.IsObject()) return false;
        it = params_it->value.FindMember(key);
        if (it == params_it->value.MemberEnd()) return false;
    }
    if (!it->value.IsBool()) return false;
    *out = it->value.GetBool();
    return true;
}

bool json_has_key(const char* json, const char* key) {
    if (!json || !key) return false;

    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseIterativeFlag>(json);
    if (doc.HasParseError() || !doc.IsObject()) return false;

    if (doc.HasMember(key)) return true;
    auto params_it = doc.FindMember("params");
    if (params_it != doc.MemberEnd() && params_it->value.IsObject()) {
        return params_it->value.HasMember(key);
    }
    return false;
}

} // namespace bridge
