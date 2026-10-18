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

}  // namespace bridge
