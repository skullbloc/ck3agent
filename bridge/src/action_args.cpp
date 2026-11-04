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

std::vector<std::string> decision_unmatched(const std::vector<std::string>& req_names,
                                            const std::vector<uint32_t>& req_hashes,
                                            const std::vector<std::string>& matched_names,
                                            const std::vector<uint32_t>& matched_hashes) {
    std::vector<std::string> out;
    for (const std::string& n : req_names) {
        bool hit = false;
        for (const std::string& m : matched_names) if (m == n) { hit = true; break; }
        if (!hit) out.push_back(n);
    }
    for (uint32_t h : req_hashes) {
        bool hit = false;
        for (uint32_t m : matched_hashes) if (m == h) { hit = true; break; }
        if (!hit) out.push_back(std::to_string(h));
    }
    return out;
}

std::string action_sub_op(const char* params_json) {
    rapidjson::Document d;
    d.Parse<rapidjson::kParseIterativeFlag>(params_json ? params_json : "");
    if (d.HasParseError() || !d.IsObject()) return {};
    const rapidjson::Value* scope = &d;
    auto pit = d.FindMember("params");
    if (pit != d.MemberEnd() && pit->value.IsObject()) scope = &pit->value;
    auto ait = scope->FindMember("action");
    if (ait == scope->MemberEnd() || !ait->value.IsString()) return {};
    return std::string(ait->value.GetString(), ait->value.GetStringLength());
}

bool call_is_read_only(const char* action_type, const char* params_json) {
    if (!action_type) return false;
    if (std::string(action_type) == "scheme_action") return action_sub_op(params_json) == "check";
    return false;
}

}  // namespace bridge
