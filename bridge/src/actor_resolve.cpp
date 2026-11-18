// actor_resolve.cpp — the one resolver of an unnamed actor (see actor_resolve.h).
// Engine-free apart from the PLAYER_CHAR_ID reader it declares, so a host test
// links it with the session sources and its own reader.
#include "actor_resolve.h"

#include "action_args.h"
#include "session.h"

#include <atomic>
#include <mutex>

namespace bridge {

extern std::atomic<uint32_t> g_controlled_primary;   // ai_eval_hook.cpp: the legacy mirror
uint32_t get_player_character_id();                  // dispatch_router.cpp: PLAYER_CHAR_ID, raw

namespace {
thread_local ActorDispatchScope* t_scope = nullptr;
}  // namespace

ActorDispatchScope* current_actor_scope() { return t_scope; }

ActorDispatchScope::ActorDispatchScope(const char* action, const char* params_json)
    : action_(action), params_(params_json), prev_(t_scope) {
    t_scope = this;
}

ActorDispatchScope::~ActorDispatchScope() { t_scope = prev_; }

void ActorDispatchScope::annotate(std::string* reply, bool handler_ok) const {
    if (!reply || !refused_rule) return;
    std::string& r = *reply;
    if (r.empty()) {
        if (handler_ok) return;
        r = "{\"success\":false,\"action\":\"";
        r += action_ ? action_ : "";
        r += "\"}";
    }
    if (r.empty() || r.back() != '}' || r.find("\"actor_rule\":") != std::string::npos) return;
    r.pop_back();
    const size_t k = r.find_last_not_of(" \t\r\n");
    if (k != std::string::npos && r[k] != '{') r += ',';
    r += "\"actor_refused\":\"";
    r += refused_reason ? refused_reason : kActorRefusalReason;
    r += "\",\"actor_rule\":\"";
    r += refused_rule;
    r += "\"";
    // rule 0: which key the verb reads, which key it was given.
    if (refused_key_read && refused_key_got) {
        r += ",\"actor_key_read\":\""; r += refused_key_read;
        r += "\",\"actor_key_got\":\""; r += refused_key_got; r += "\"";
    }
    r += "}";
}

ActorUse current_actor_use() {
    const ActorDispatchScope* s = t_scope;
    if (!s || !s->action()) return ActorUse::Legacy;
    // The observe gate's own classification (dispatch_router.cpp dispatch_action).
    if (action_is_mutation(s->action()) && !call_is_read_only(s->action(), s->params()))
        return ActorUse::Write;
    return ActorUse::ReadSelf;
}

ActorFacts gather_actor_facts(uint32_t explicit_id, LegacyChain chain) {
    ActorFacts f;
    f.explicit_id = explicit_id;
    if (Session* s = current_session()) {
        f.have_session = true;
        f.declared_seat = s->declared_seat.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(s->mutex);
        f.session_primary = s->primary_character_id;
    }
    f.seats_in_play = g_session_registry.any_seat_declared();
    f.global_mirror = g_controlled_primary.load(std::memory_order_relaxed);
    if (chain == LegacyChain::MirrorThenPlayer && explicit_id == 0 && f.global_mirror == 0)
        f.player_raw = get_player_character_id();
    return f;
}

ActorPick resolve_actor_id(uint32_t explicit_id, LegacyChain chain,
                           const char* key_read, const char* key_unread) {
    ActorFacts facts = gather_actor_facts(explicit_id, chain);
    facts.key_read = key_read;
    facts.key_unread = key_unread;
#ifdef CK3_ACTOR_PRE_FIX
    // NEGATIVE CONTROL ONLY (test_actor_resolve_pre_fix_control): every call
    // keeps its handler's historical chain, which is what the tree did before
    // this module — the chains in legacy_pick are transcriptions of it.
    const ActorPick p = pick_actor(facts, ActorUse::Legacy, chain);
#else
    const ActorPick p = pick_actor(facts, current_actor_use(), chain);
#endif
    if (ActorDispatchScope* s = t_scope) {
        s->last_reason = p.reason;
        if (p.rule) {
            s->refused_rule = p.rule;
            s->refused_reason = p.reason;
            s->refused_key_read = p.key_read;
            s->refused_key_got = p.key_got;
        }
    }
    return p;
}

const char* last_actor_refusal() {
    return t_scope ? t_scope->last_reason : nullptr;
}

}  // namespace bridge
