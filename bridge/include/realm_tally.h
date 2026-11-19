// realm_tally.h — query_realm's `realm_total_counties` for MANY rulers from ONE
// walk of the character table (wave 19).
//
// count_realm_counties(R) (query_handlers.cpp) is R's own demesne counties plus
// the demesne counties of every OTHER living, landed, AI-enabled character whose
// independent top liege is R; a holder whose liege walk cannot be made (top 0)
// is counted as unresolved. It used to walk the whole character table (~63,000
// entries, ~0.9 s on the game thread in the re-pilot's trace) for EVERY read, so
// a king's observe that read his 32 vassals' realms paid 32 walks for one paused
// world. The walk is now taken
// once into a Tally and every ruler's count is answered from it; a single read
// builds its own Tally, the batched read (query_realm `character_ids`) builds one
// for the whole batch. One definition: both paths call total() / unresolved_for().
//
// Engine-free so a host test reaches it (bridge/test/test_realm_tally.cpp), which
// checks it against the per-ruler loop it replaced.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include <rapidjson/document.h>

namespace realm_tally {

//: The most rulers one batched query_realm answers; a longer list is refused
//: whole (never truncated). The server's VASSAL_READ_CAP (pushed_warnings.py)
//: must not exceed it (test_pushed_warnings checks the mirror).
constexpr int kBatchMax = 64;

struct Tally {
    //: counties of the walked holders whose top liege is the key
    std::unordered_map<uint32_t, int> by_top;
    //: the part of by_top[id] that is id's OWN entry (an independent holder walks to himself)
    std::unordered_map<uint32_t, int> self_in_top;
    //: how many of id's own entries could not be walked (top 0)
    std::unordered_map<uint32_t, int> self_unresolved;
    int unresolved = 0;   // walked holders whose top liege could not be made
    int walks = 0;        // character-table walks this Tally took (1 when built)

    void add(uint32_t id, int counties, uint32_t top) {
        id &= 0xFFFFFF;
        top &= 0xFFFFFF;
        if (top == 0) {
            unresolved++;
            self_unresolved[id]++;
            return;
        }
        by_top[top] += counties;
        if (top == id) self_in_top[id] += counties;
    }

    // R's realm county total, given R's own demesne count: R's demesne plus every
    // OTHER walked holder whose top liege is R (R's own entry is his demesne,
    // already counted).
    int total(uint32_t ruler_id, int demesne) const {
        ruler_id &= 0xFFFFFF;
        return demesne + at(by_top, ruler_id) - at(self_in_top, ruler_id);
    }

    // The walked holders OTHER than R whose top liege could not be made.
    int unresolved_for(uint32_t ruler_id) const {
        return unresolved - at(self_unresolved, ruler_id & 0xFFFFFF);
    }

private:
    static int at(const std::unordered_map<uint32_t, int>& m, uint32_t k) {
        auto it = m.find(k);
        return it == m.end() ? 0 : it->second;
    }
};

// One walk. `each(cb)` calls cb(id, alive, has_realm, handle) for every character
// the count considers (the bridge: enumerate_ai_characters, alive AI-enabled
// characters); `demesne_of(handle)` its directly-held county count;
// `top_of(handle)` its independent top liege (0: the walk could not be made).
// A holder of no county is skipped before its liege walk, as before.
template <class Each, class DemesneOf, class TopOf>
Tally build(Each each, DemesneOf demesne_of, TopOf top_of) {
    Tally t;
    t.walks = 1;
    each([&](uint32_t id, bool alive, bool has_realm, auto handle) {
        if (!alive || !has_realm) return;
        const int counties = demesne_of(handle);
        if (counties == 0) return;
        t.add(id, counties, top_of(handle));
    });
    return t;
}

// The batched read's ids: `character_ids`, top level or under "params" (the
// envelope path hands a handler the whole inner action), a JSON array of 1 to
// kBatchMax positive integers (numeric strings accepted, as target_ids are).
// Returns nullptr and fills `ids` (24-bit, in order), or the refusal code; the
// whole list is refused, never a part of it read:
//   ambiguous_character_ids  character_id given beside character_ids
//   invalid_character_ids    not an array, empty, or an element not a positive id
//   too_many_character_ids   more than kBatchMax
inline const char* parse_batch(const char* params_json, std::vector<uint32_t>* ids) {
    ids->clear();
    rapidjson::Document doc;
    doc.Parse(params_json ? params_json : "");
    if (doc.HasParseError() || !doc.IsObject()) return "invalid_character_ids";
    const rapidjson::Value* obj = &doc;
    if (!doc.HasMember("character_ids")) {
        auto pit = doc.FindMember("params");
        if (pit == doc.MemberEnd() || !pit->value.IsObject()) return "invalid_character_ids";
        obj = &pit->value;
    }
    if (obj->HasMember("character_id")) return "ambiguous_character_ids";
    auto it = obj->FindMember("character_ids");
    if (it == obj->MemberEnd() || !it->value.IsArray() || it->value.Empty()) return "invalid_character_ids";
    const auto& arr = it->value;
    if (arr.Size() > static_cast<rapidjson::SizeType>(kBatchMax)) return "too_many_character_ids";
    for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
        const auto& el = arr[i];
        long long id = -1;
        if (el.IsInt64()) id = el.GetInt64();
        else if (el.IsUint64()) id = static_cast<long long>(el.GetUint64() & 0xFFFFFFFFull);
        else if (el.IsString()) {
            char* end = nullptr;
            long long v = std::strtoll(el.GetString(), &end, 10);
            if (end != el.GetString() && end && *end == '\0') id = v;
        }
        if (id <= 0 || id > 0xFFFFFF) {
            ids->clear();
            return "invalid_character_ids";
        }
        ids->push_back(static_cast<uint32_t>(id));
    }
    return nullptr;
}

}  // namespace realm_tally
