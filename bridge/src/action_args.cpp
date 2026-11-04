// action_args.cpp — see action_args.h for why this exists (ck-6kvq.15).
#include "action_args.h"

namespace bridge {

const rapidjson::Value& action_args(const rapidjson::Value& inner, const char* key) {
    if (!inner.IsObject() || !key) return inner;
    if (inner.HasMember(key)) return inner;
    auto pit = inner.FindMember("params");
    if (pit != inner.MemberEnd() && pit->value.IsObject() && pit->value.HasMember(key))
        return pit->value;
    return inner;
}

bool action_has(const rapidjson::Value& inner, const char* key) {
    if (!inner.IsObject() || !key) return false;
    return action_args(inner, key).HasMember(key);
}

bool decision_request(const char* params_json, std::vector<uint32_t>* hashes,
                      std::vector<std::string>* names) {
    rapidjson::Document d;
    if (!params_json || d.Parse(params_json).HasParseError() || !d.IsObject()) return false;
    const rapidjson::Value& args = action_args(d, "decisions");
    if (!args.HasMember("decisions") || !args["decisions"].IsArray()) return true;
    for (auto& v : args["decisions"].GetArray()) {
        if (v.IsString())      { if (names) names->emplace_back(v.GetString(), v.GetStringLength()); }
        else if (v.IsUint())   { if (hashes) hashes->push_back(v.GetUint()); }
        else if (v.IsInt())    { if (hashes) hashes->push_back(static_cast<uint32_t>(v.GetInt())); }
        else if (v.IsUint64()) { if (hashes) hashes->push_back(static_cast<uint32_t>(v.GetUint64())); }
    }
    return true;
}

}  // namespace bridge
