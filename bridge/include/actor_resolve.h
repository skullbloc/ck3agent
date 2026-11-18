// actor_resolve.h — THE one place an unnamed actor is resolved (wave 12).
//
// WHY. v4 seats one king and several dukes in one game, each its own bridge
// session. A request that names no actor used to be resolved by whatever chain
// its handler carried: get_controlled_character() (this session's primary, else
// the GLOBAL primary mirror), or, at nine query sites and the decision eval, the
// GLOBAL mirror first and then PLAYER_CHAR_ID. The global mirror is the primary
// of the session that claimed FIRST (session.cpp rebuild_legacy_mirrors_locked),
// so a duke's id-less read answered about the king, and a write by a session
// holding nothing acted AS another seat's ruler — a write the ownership gate
// (session.cpp validate_actor_ownership) refuses `owned_by_other` when the same
// id is NAMED. Every one of those chains now asks this module.
//
// THE RULE (pick_actor; pure, so a host test can ask it every case):
//   1. An actor the request names is the actor (the handler's own parse; the
//      dispatch ownership gate has already vetted it for a mutation).
//   2. A WRITE (the tree's own classification: action_is_mutation and not
//      call_is_read_only — exactly the observe gate's) resolves to THIS
//      session's own primary, or is REFUSED `no_actor`. It never reads the
//      global mirror or the player. With no session at all (a client that never
//      handshook: the historical N=1 raw-socket layout, whose own claim IS the
//      mirror) it keeps its chain, unless some session has declared a seat, in
//      which case the mirror is a seat's ruler and it is refused too.
//   3. A READ by a session that DECLARED A SEAT (`episode_seat`)
//      resolves to that session's own primary, or is REFUSED `no_actor`.
//   4. Every other read — no seat declared on this session — and every call
//      outside a dispatched action keeps its handler's historical chain BYTE FOR
//      BYTE (LegacyChain names it). That is the single-player layout's promise.
//   0. (wave 15; asked before 2-4) A WRITE that names no actor
//      under the key its handler reads, while the request names one under the
//      OTHER actor key (`character_id` where the handler reads `actor_id`, or
//      the reverse), is REFUSED `actor_key_not_read`, in every layout. Live
//      (q13 qf4, C2): set_council_task got {"character_id": <ruler>}, read
//      `actor_id`, and wrote on the session's primary, answering success-shaped
//      `no_change_already_set` about a ruler the caller never named. The
//      handler says which key it reads (resolve_actor: actor_id;
//      resolve_character: character_id; queue_intent: actor_id); a handler
//      that reads BOTH keys (send_decision; appoint_councillor, whose
//      character_id is the appointee and which requires actor_id) asks with no
//      key and is never refused by this rule. Reads are untouched.
//
// A refusal is recorded for the dispatch (ActorDispatchScope): actor_refusal()
// answers it instead of "actor_not_found", and the dispatch adds
// "actor_refused":"no_actor","actor_rule":<rule> to the reply.
//
// THE CENSUS. bridge/test/check_actor_resolution.py fails when a function reads
// g_controlled_primary, PLAYER_CHAR_ID or a session's primary_character_id
// outside this module and its declared, reasoned non-resolver uses.
#pragma once

#include <cstdint>
#include <string>

namespace bridge {

class Session;

// What the call does with the actor it resolves. Derived from the dispatched
// action, never passed by a handler (one classification, the observe gate's).
enum class ActorUse : uint8_t {
    Legacy,     // no dispatched action on this thread (tick hooks, drains, telemetry)
    ReadSelf,   // a read-only action
    Write,      // a mutation
};

// The handler's historical chain for an unnamed actor, kept exactly for rule 4.
enum class LegacyChain : uint8_t {
    SessionThenMirror,   // get_controlled_character(): session primary, else the global mirror
    MirrorThenPlayer,    // the query_* sites: global mirror, else PLAYER_CHAR_ID (masked 24 bits)
    MirrorOnly,          // queue_intent, query_marriage_candidates, get_tick_info
};

enum class ActorSource : uint8_t {
    Explicit, SessionPrimary, GlobalMirror, PlayerGlobal, None, Refused,
};

// Everything the rule reads, gathered once (gather_actor_facts) so pick_actor
// is pure.
struct ActorFacts {
    uint32_t explicit_id     = 0;      // the handler's parsed actor; 0 = none named
    bool     have_session    = false;  // a session is installed on this request
    bool     declared_seat   = false;  // that session declared `episode_seat`
    uint32_t session_primary = 0;      // that session's primary_character_id
    bool     seats_in_play   = false;  // ANY live session declared a seat
    uint32_t global_mirror   = 0;      // g_controlled_primary
    uint32_t player_raw      = 0;      // PLAYER_CHAR_ID as read (0xFFFFFFFF in observer)
    // Rule 0: the actor key the handler reads, and the OTHER
    // actor key when the request names an actor (> 0) under it; nullptr = none.
    const char* key_read     = nullptr;
    const char* key_unread   = nullptr;
};

struct ActorPick {
    uint32_t    id     = 0;
    ActorSource source = ActorSource::None;
    const char* reason = nullptr;   // non-null iff Refused: "no_actor" or "actor_key_not_read"
    const char* rule   = nullptr;   // non-null iff Refused: which rule refused
    const char* key_read = nullptr; // rule 0 only: the key the handler reads
    const char* key_got  = nullptr; // rule 0 only: the key the request named its actor under
};

// Typed refusal vocabulary (one spelling each; the census and the tests read them).
inline constexpr const char* kActorRefusalReason   = "no_actor";
inline constexpr const char* kRuleWriteNeedsActor  = "write_requires_actor";
inline constexpr const char* kRuleSeatReadNeedsActor = "seat_read_requires_actor";
inline constexpr const char* kActorKeyNotRead      = "actor_key_not_read";
inline constexpr const char* kRuleActorUnderUnreadKey = "write_names_actor_under_unread_key";
inline constexpr const char* kActorKeyActorId      = "actor_id";
inline constexpr const char* kActorKeyCharacterId  = "character_id";

// The other actor key: actor_id <-> character_id; nullptr for any other key.
inline const char* actor_key_counterpart(const char* read_key) {
    if (!read_key) return nullptr;
    const std::string k(read_key);
    if (k == kActorKeyActorId) return kActorKeyCharacterId;
    if (k == kActorKeyCharacterId) return kActorKeyActorId;
    return nullptr;
}

// get_controlled_character's own test of a primary: 0 and 0xFFFFFFFF are none.
inline bool actor_primary_usable(uint32_t id) { return id != 0 && id != 0xFFFFFFFFu; }

inline ActorPick legacy_pick(const ActorFacts& f, LegacyChain chain) {
    ActorPick p;
    switch (chain) {
    case LegacyChain::SessionThenMirror:
        if (f.have_session && actor_primary_usable(f.session_primary)) {
            p.id = f.session_primary; p.source = ActorSource::SessionPrimary; return p;
        }
        if (actor_primary_usable(f.global_mirror)) {
            p.id = f.global_mirror; p.source = ActorSource::GlobalMirror; return p;
        }
        return p;                                   // None: the handler answers its own "not found"
    case LegacyChain::MirrorThenPlayer: {
        uint32_t ctrl = f.global_mirror;
        p.source = ActorSource::GlobalMirror;
        if (ctrl == 0) { ctrl = f.player_raw; p.source = ActorSource::PlayerGlobal; }
        p.id = ctrl & 0xFFFFFFu;
        if (p.id == 0) p.source = ActorSource::None;
        return p;
    }
    case LegacyChain::MirrorOnly:
        p.id = f.global_mirror;
        p.source = (p.id == 0) ? ActorSource::None : ActorSource::GlobalMirror;
        return p;
    }
    return p;
}

inline ActorPick pick_actor(const ActorFacts& f, ActorUse use, LegacyChain chain) {
    ActorPick p;
    if (f.explicit_id != 0) { p.id = f.explicit_id; p.source = ActorSource::Explicit; return p; }
    auto refuse = [&](const char* rule) {
        ActorPick r; r.source = ActorSource::Refused;
        r.reason = kActorRefusalReason; r.rule = rule; return r;
    };
    auto own = [&]() {
        ActorPick r; r.id = f.session_primary; r.source = ActorSource::SessionPrimary; return r;
    };
    if (use == ActorUse::Write && f.key_unread) {          // rule 0
        ActorPick r = refuse(kRuleActorUnderUnreadKey);
        r.reason = kActorKeyNotRead; r.key_read = f.key_read; r.key_got = f.key_unread;
        return r;
    }
    switch (use) {
    case ActorUse::Legacy:
        return legacy_pick(f, chain);
    case ActorUse::ReadSelf:
        if (f.have_session && f.declared_seat)
            return actor_primary_usable(f.session_primary) ? own() : refuse(kRuleSeatReadNeedsActor);
        return legacy_pick(f, chain);
    case ActorUse::Write:
        if (f.have_session)
            return actor_primary_usable(f.session_primary) ? own() : refuse(kRuleWriteNeedsActor);
        if (f.seats_in_play) return refuse(kRuleWriteNeedsActor);
        {
            // No session, no seat anywhere: the N=1 raw-socket layout, whose own
            // claim is the mirror. Never the player for a write.
            ActorPick m = legacy_pick(f, chain);
            if (m.source == ActorSource::PlayerGlobal) m = ActorPick{};
            return m;
        }
    }
    return p;
}

// The use the CURRENT dispatched action implies (Legacy outside a dispatch).
ActorUse current_actor_use();

// Gather the facts for this thread's request (current_session, the registry,
// g_controlled_primary; PLAYER_CHAR_ID is read only when `chain` can reach it).
ActorFacts gather_actor_facts(uint32_t explicit_id, LegacyChain chain);

// THE resolver every handler calls for an actor id. Records a refusal on the
// current dispatch scope (or clears the last one on success). `key_read` /
// `key_unread` feed rule 0 (ActorFacts); a handler computes them with
// unread_actor_key() (action_dispatch_internal.h), never by hand.
ActorPick resolve_actor_id(uint32_t explicit_id, LegacyChain chain,
                           const char* key_read = nullptr, const char* key_unread = nullptr);

// The refusal reason recorded by the LAST resolve on this thread inside the
// current dispatch, or nullptr.
const char* last_actor_refusal();

// Installed by dispatch_action around a registered handler. Nests.
class ActorDispatchScope {
public:
    ActorDispatchScope(const char* action, const char* params_json);
    ~ActorDispatchScope();
    // Adds "actor_refused"/"actor_rule" to the reply when a resolve in this
    // scope refused (and the reply is a JSON object that does not say so yet).
    // A failed handler that left no reply gets {"success":false,"action":..}
    // plus the two keys (what dispatch_action_with_response would fill, typed).
    void annotate(std::string* reply, bool handler_ok) const;
    const char* action() const { return action_; }
    const char* params() const { return params_; }
    const char* refused_rule = nullptr;   // the last refusal in this scope
    const char* refused_reason = nullptr; // its reason (no_actor / actor_key_not_read)
    const char* refused_key_read = nullptr;  // rule 0: the key the handler reads
    const char* refused_key_got  = nullptr;  // rule 0: the key the request named its actor under
    const char* last_reason  = nullptr;   // the last resolve's refusal, cleared on success
private:
    const char* action_;
    const char* params_;
    ActorDispatchScope* prev_;
};

ActorDispatchScope* current_actor_scope();

}  // namespace bridge
