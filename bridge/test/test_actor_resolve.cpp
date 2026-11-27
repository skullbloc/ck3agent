// test_actor_resolve.cpp — an unnamed actor resolves by ONE rule.
//
// v4 seats a king and several dukes in one game, one bridge session each. The
// superseded tree resolved a request that names no actor by its handler's own
// chain: get_controlled_character() (this session's primary, else the GLOBAL
// mirror), or — at nine query sites, the decision eval, query_marriage_candidates
// and queue_intent — the GLOBAL mirror first. The mirror is the first claimer's
// primary, so a duke's id-less read answered about the king and a write by a
// session holding nothing acted as another seat's ruler.
//
// Two halves:
//   A. pick_actor, the pure rule, case by case — including a grid that pins
//      the no-seat read to TRANSCRIPTIONS of the superseded code (byte for byte);
//   B. the same rule through the REAL dispatch_action of the REAL
//      dispatch_router.cpp: the BRIDGE_ACTION macro's actor scope classifies
//      the call (query_* reads, set_* writes) and annotates the reply. The
//      handlers are strong stand-ins doing exactly what the real sites do
//      (one resolve_actor_id / get_controlled_character call), since the real
//      ones need the game; check_actor_resolution.py holds the real sites to it.
//
// NEGATIVE CONTROL: built with -DCK3_ACTOR_PRE_FIX_CONTROL against
// actor_resolve.cpp built -DCK3_ACTOR_PRE_FIX (every call keeps its historical
// chain). It exits 0 only when EXACTLY kPreFixFails fail.
//
// Exit 0 pass, 1 fail.
#include "action_dispatch_internal.h"
#include "actor_resolve.h"
#include "session.h"
#include "pipeline.h"
#include "intent_queue.h"
#include "name_tables.h"
#include "snapshot.h"
#include "episode_policy.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

// Host stand-ins for the library globals dispatch_router.cpp expects (as
// test_release_handler.cpp).
namespace bridge {
uintptr_t                g_base_addr = 0;
FILE*                    g_logfile = nullptr;
std::vector<MemRange>    g_valid_ranges;
std::shared_mutex        g_ranges_mutex;
std::mutex               g_controlled_mutex;
std::vector<uint32_t>    g_controlled_chars;
std::atomic<uint32_t>    g_controlled_primary{0};
std::atomic<uint32_t>    g_controlled_heir{0};
std::atomic<bool>        g_agent_mode{false};
std::atomic<uint64_t>    g_tick_id{0};
std::atomic<uint64_t>    g_cb_query_busy_count{0};
std::atomic<bool>        g_war_stance_record_walk_enabled{false};
IntentQueue              g_intent_queue;
size_t IntentQueue::pending_count() { return 0; }
void council_lock_remove(uint32_t) {}
bool pipeline_owns_clock()       { return false; }
bool is_clock_verb(const char*)  { return false; }
uint64_t pipeline_current_flush_id() { return 0; }
PipelineMode pipeline_mode() { return PipelineMode::Legacy; }
void clock_write(ClockWrite, ClockOrigin, int) {}
const char* session_state_string() { return "test"; }
std::string get_cached_character_name(uint32_t) { return "(test)"; }
std::string enumerate_interactions() { return "[]"; }
std::string enumerate_decisions()    { return "[]"; }
bool get_client_lock_info(ClientLockInfo*) { return false; }
bool is_game_state_query(const char*) { return false; }
const char* offset_validation_group_name(int) { return ""; }
OffsetValidationResult validate_offsets() { return OffsetValidationResult{}; }

// ---- strong stand-ins for three engine-bound handlers ----------------------
// Each does what its real site does with an unnamed actor and records it.
ActorPick g_seen{};
bool g_seen_set = false;
const char* g_seen_refusal = nullptr;

// query_handlers.cpp do_query_valid_cbs: actor_id, else resolve_actor_id(0, MirrorThenPlayer).
bool do_query_valid_cbs(const char* params_json, std::string* out) {
    int actor_id = json_get_int(params_json, "actor_id");
    g_seen = resolve_actor_id(actor_id > 0 ? static_cast<uint32_t>(actor_id) : 0,
                              LegacyChain::MirrorThenPlayer);
    g_seen_set = true;
    if (g_seen.id == 0) { if (out) *out = R"({"error":"no_actor_available"})"; return false; }
    if (out) *out = R"({"success":true})";
    return true;
}
// personality.cpp do_set_ai_personality: resolve_character (the REAL one,
// dispatch_router.cpp), which reads character_id.
bool do_set_ai_personality(const char* params_json, std::string* out) {
    g_seen = resolve_actor_id(0, LegacyChain::SessionThenMirror);   // what get_controlled_character asks
    g_seen_set = true;
    void* actor = resolve_character(params_json);                   // the real one (dispatch_router.cpp)
    g_seen_refusal = actor_refusal(actor);
    if (g_seen_refusal) { if (out) *out = actor_refusal_reply("set_ai_personality", g_seen_refusal); return false; }
    return true;
}
// action_handlers.cpp do_queue_intent: actor_id, else resolve_actor_id(0, MirrorOnly).
bool do_queue_intent(const char* /*params_json*/, std::string* out) {
    g_seen = resolve_actor_id(0, LegacyChain::MirrorOnly);
    g_seen_set = true;
    if (g_seen.id == 0) {
        if (out) *out = g_seen.reason ? R"({"success":false,"action":"queue_intent","reason":"no_actor"})"
                                      : R"({"success":false,"action":"queue_intent","reason":"actor_id_required"})";
        return false;
    }
    return true;
}
// action_handlers.cpp do_set_council_task: resolve_actor (the
// REAL one, dispatch_router.cpp), which reads actor_id; a refused actor answers
// actor_refusal's reason and writes nothing. g_council_wrote says whether the
// stand-in got as far as the write (the resolve did not refuse).
bool g_council_wrote = false;
bool do_set_council_task(const char* params_json, std::string* out) {
    g_council_wrote = false;
    (void)resolve_actor(params_json);
    g_seen_refusal = last_actor_refusal();
    if (g_seen_refusal) {
        if (out) *out = actor_refusal_reply("set_council_task", actor_refusal(nullptr));
        return false;
    }
    g_council_wrote = true;
    if (out) *out = R"({"success":true,"action":"set_council_task"})";
    return true;
}
// query_handlers.cpp do_query_character: resolve_character -> get_controlled_character;
// a null character fails with NO reply (the dispatch fills it).
bool do_query_character(const char* /*params_json*/, std::string* /*out*/) {
    g_seen = resolve_actor_id(0, LegacyChain::SessionThenMirror);
    g_seen_set = true;
    void* character = get_controlled_character();
    return character != nullptr;
}
}  // namespace bridge

using namespace bridge;

namespace {
int failures = 0;
std::set<std::string> failed;
void row(const char* n, bool ok, const std::string& d = std::string()) {
    std::printf("%-58s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) { ++failures; failed.insert(n); }
}
std::string src(ActorSource s) {
    switch (s) {
    case ActorSource::Explicit: return "explicit"; case ActorSource::SessionPrimary: return "session";
    case ActorSource::GlobalMirror: return "mirror"; case ActorSource::PlayerGlobal: return "player";
    case ActorSource::None: return "none"; case ActorSource::Refused: return "refused";
    }
    return "?";
}
std::string show(const ActorPick& p) {
    return "id=" + std::to_string(p.id) + " src=" + src(p.source) + (p.rule ? std::string(" rule=") + p.rule : "");
}

// ---- transcriptions of the SUPERSEDED code, verbatim in effect ----------------
// dispatch_router.cpp get_controlled_character (the id it looked up, 0 = nullptr).
uint32_t old_get_controlled(bool have_session, uint32_t session_primary, uint32_t mirror) {
    uint32_t cid = 0;
    if (have_session) cid = session_primary;
    if (cid == 0 || cid == 0xFFFFFFFF) cid = mirror;
    if (cid == 0 || cid == 0xFFFFFFFF) return 0;
    return cid;
}
// query_handlers.cpp do_query_valid_cbs & seven siblings (and, same in effect,
// enumerate_decisions_annotated): mirror, else PLAYER_CHAR_ID, masked.
uint32_t old_query_site(uint32_t mirror, uint32_t player_raw) {
    uint32_t ctrl = mirror;
    if (ctrl == 0) ctrl = player_raw;
    return ctrl & 0xFFFFFF;
}
// action_handlers.cpp do_queue_intent / query_marriage_candidates: the mirror.
uint32_t old_mirror_only(uint32_t mirror) { return mirror; }

void part_a_pure() {
    const uint32_t KING = 7001, DUKE = 7002, PLAYER = 9001;
    ActorFacts f;
    // explicit wins in every use and chain
    f.explicit_id = 4242; f.have_session = true; f.declared_seat = true; f.session_primary = DUKE; f.global_mirror = KING;
    bool all = true;
    for (ActorUse u : {ActorUse::Legacy, ActorUse::ReadSelf, ActorUse::Write})
        for (LegacyChain c : {LegacyChain::SessionThenMirror, LegacyChain::MirrorThenPlayer, LegacyChain::MirrorOnly}) {
            ActorPick p = pick_actor(f, u, c);
            all = all && p.id == 4242 && p.source == ActorSource::Explicit;
        }
    row("a01_named_actor_wins_everywhere", all);

    // the duke/king case: a seat's read answers about the seat, every chain
    f = ActorFacts{}; f.have_session = true; f.declared_seat = true; f.session_primary = DUKE; f.global_mirror = KING; f.player_raw = PLAYER;
    all = true;
    for (LegacyChain c : {LegacyChain::SessionThenMirror, LegacyChain::MirrorThenPlayer, LegacyChain::MirrorOnly}) {
        ActorPick p = pick_actor(f, ActorUse::ReadSelf, c);
        all = all && p.id == DUKE && p.source == ActorSource::SessionPrimary;
    }
    row("a02_seat_read_is_its_own_primary_not_the_mirror", all);

    f.session_primary = 0;
    ActorPick p = pick_actor(f, ActorUse::ReadSelf, LegacyChain::MirrorThenPlayer);
    row("a03_seat_read_without_primary_refused", p.source == ActorSource::Refused && p.id == 0
        && std::string(p.reason) == "no_actor" && std::string(p.rule) == kRuleSeatReadNeedsActor, show(p));

    // writes: own primary or refused, seat or not
    f = ActorFacts{}; f.have_session = true; f.session_primary = DUKE; f.global_mirror = KING;
    p = pick_actor(f, ActorUse::Write, LegacyChain::MirrorOnly);
    row("a04_write_is_own_primary_not_the_mirror", p.id == DUKE && p.source == ActorSource::SessionPrimary, show(p));
    f.session_primary = 0;
    all = true;
    for (bool seat : {false, true}) {
        f.declared_seat = seat;
        for (LegacyChain c : {LegacyChain::SessionThenMirror, LegacyChain::MirrorThenPlayer, LegacyChain::MirrorOnly}) {
            p = pick_actor(f, ActorUse::Write, c);
            all = all && p.source == ActorSource::Refused && std::string(p.rule) == kRuleWriteNeedsActor;
        }
    }
    row("a05_write_without_own_primary_refused_never_mirror", all);

    // session-less write: the N=1 raw-socket layout keeps its mirror; not with a seat in play
    f = ActorFacts{}; f.global_mirror = KING;
    p = pick_actor(f, ActorUse::Write, LegacyChain::SessionThenMirror);
    row("a06_sessionless_write_no_seats_keeps_mirror", p.id == KING && p.source == ActorSource::GlobalMirror, show(p));
    f.seats_in_play = true;
    p = pick_actor(f, ActorUse::Write, LegacyChain::SessionThenMirror);
    row("a07_sessionless_write_with_seats_refused", p.source == ActorSource::Refused, show(p));
    f = ActorFacts{}; f.player_raw = PLAYER;
    p = pick_actor(f, ActorUse::Write, LegacyChain::MirrorThenPlayer);
    row("a08_write_never_the_player", p.id == 0 && p.source != ActorSource::PlayerGlobal, show(p));

    // BYTE FOR BYTE: a read with no seat declared (and every Legacy call) gives
    // what the superseded code gave, over a grid of facts.
    const uint32_t ids[] = {0u, KING, DUKE, 0xFFFFFFFFu, 0x01001234u};
    const uint32_t players[] = {0u, PLAYER, 0xFFFFFFFFu};
    long checked = 0, bad = 0;
    std::string first_bad;
    for (bool hs : {false, true}) for (uint32_t sp : ids) for (uint32_t m : ids) for (uint32_t pl : players)
    for (ActorUse u : {ActorUse::ReadSelf, ActorUse::Legacy}) {
        ActorFacts g; g.have_session = hs; g.declared_seat = false; g.session_primary = sp;
        g.global_mirror = m; g.player_raw = pl; g.seats_in_play = true;   // others' seats do not matter
        const uint32_t want[3] = {old_get_controlled(hs, sp, m), old_query_site(m, pl), old_mirror_only(m)};
        const LegacyChain cs[3] = {LegacyChain::SessionThenMirror, LegacyChain::MirrorThenPlayer, LegacyChain::MirrorOnly};
        for (int i = 0; i < 3; ++i) {
            ++checked;
            ActorPick q = pick_actor(g, u, cs[i]);
            if (q.id != want[i] || q.reason) {
                ++bad;
                if (first_bad.empty()) first_bad = "chain " + std::to_string(i) + " sp=" + std::to_string(sp)
                    + " m=" + std::to_string(m) + " pl=" + std::to_string(pl) + " got " + show(q)
                    + " want " + std::to_string(want[i]);
            }
        }
    }
    row("a09_no_seat_read_byte_for_byte_with_pre_fix_chains", bad == 0,
        std::to_string(checked) + " cases, " + std::to_string(bad) + " differ " + first_bad);

    // Rule 0: a write whose actor is named under the key the
    // handler does not read is refused, in every layout and chain, naming both keys.
    all = true;
    std::string first;
    for (bool hs : {false, true}) for (bool seat : {false, true}) for (bool others : {false, true})
    for (uint32_t sp : {0u, DUKE}) for (uint32_t m : {0u, KING})
    for (LegacyChain c : {LegacyChain::SessionThenMirror, LegacyChain::MirrorThenPlayer, LegacyChain::MirrorOnly}) {
        ActorFacts g; g.have_session = hs; g.declared_seat = seat; g.seats_in_play = others;
        g.session_primary = sp; g.global_mirror = m; g.player_raw = PLAYER;
        g.key_read = kActorKeyActorId; g.key_unread = kActorKeyCharacterId;
        ActorPick q = pick_actor(g, ActorUse::Write, c);
        const bool ok = q.id == 0 && q.source == ActorSource::Refused && q.reason
            && std::string(q.reason) == "actor_key_not_read"
            && std::string(q.rule) == "write_names_actor_under_unread_key"
            && q.key_read && std::string(q.key_read) == "actor_id"
            && q.key_got && std::string(q.key_got) == "character_id";
        if (!ok && first.empty()) first = show(q);
        all = all && ok;
    }
    row("a10_write_with_actor_under_unread_key_refused_everywhere", all, first);

    // the reverse pair (resolve_character reads character_id, given actor_id)
    f = ActorFacts{}; f.have_session = true; f.session_primary = DUKE;
    f.key_read = kActorKeyCharacterId; f.key_unread = kActorKeyActorId;
    p = pick_actor(f, ActorUse::Write, LegacyChain::SessionThenMirror);
    row("a11_reverse_pair_refused_naming_both_keys", p.source == ActorSource::Refused
        && std::string(p.key_read) == "character_id" && std::string(p.key_got) == "actor_id", show(p));

    // reads and calls outside a dispatch are untouched: rule 0 is a write's
    all = true;
    for (ActorUse u : {ActorUse::ReadSelf, ActorUse::Legacy}) {
        f = ActorFacts{}; f.have_session = true; f.session_primary = DUKE; f.global_mirror = KING;
        f.key_read = kActorKeyActorId; f.key_unread = kActorKeyCharacterId;
        ActorFacts g = f; g.key_read = nullptr; g.key_unread = nullptr;
        for (LegacyChain c : {LegacyChain::SessionThenMirror, LegacyChain::MirrorThenPlayer, LegacyChain::MirrorOnly}) {
            ActorPick a = pick_actor(f, u, c), b = pick_actor(g, u, c);
            all = all && a.id == b.id && a.source == b.source && a.reason == b.reason;
        }
    }
    row("a12_reads_and_legacy_untouched_by_rule_0", all);

    // a named actor still wins; no unread key = the write rule as before
    f = ActorFacts{}; f.explicit_id = 4242; f.key_read = kActorKeyActorId; f.key_unread = kActorKeyCharacterId;
    p = pick_actor(f, ActorUse::Write, LegacyChain::SessionThenMirror);
    row("a13_named_actor_wins_over_rule_0", p.id == 4242 && p.source == ActorSource::Explicit, show(p));
    f = ActorFacts{}; f.have_session = true; f.session_primary = DUKE; f.key_read = kActorKeyActorId;
    p = pick_actor(f, ActorUse::Write, LegacyChain::SessionThenMirror);
    row("a14_no_unread_key_write_is_own_primary", p.id == DUKE && p.source == ActorSource::SessionPrimary, show(p));

    // the counterpart is exactly the two actor keys
    row("a15_counterpart_is_the_two_actor_keys",
        std::string(actor_key_counterpart("actor_id")) == "character_id"
        && std::string(actor_key_counterpart("character_id")) == "actor_id"
        && actor_key_counterpart("target_id") == nullptr && actor_key_counterpart(nullptr) == nullptr);
}

// ---- part B: through the real dispatch_action ------------------------------
Session* make(const char* name, bool seat) {
    Session* s = g_session_registry.create(name, Session::Mode::CONTROL);
    if (seat) s->declared_seat.store(true);
    return s;
}
std::string run(Session* s, const char* action, const char* params) {
    ClientContext ctx; ctx.session = s;
    ClientContextGuard guard(&ctx);
    g_seen_set = false; g_seen = ActorPick{}; g_seen_refusal = nullptr; g_council_wrote = false;
    std::string out;
    dispatch_action(action, params, &out);
    return out;
}

void part_b_dispatch() {
    const uint32_t KING = 7001, DUKE = 7002;
    std::string why;
    Session* king = make("seat-king", true);
    row("b00_king_claims", g_session_registry.claim_character(king, KING, &why), why);
    Session* duke = make("seat-duke", true);
    row("b00_duke_claims", g_session_registry.claim_character(duke, DUKE, &why), why);
    const uint32_t mirror = g_controlled_primary.load();
    row("b00_global_mirror_is_the_first_claimer", mirror == KING, std::to_string(mirror));

    // the hazard: the duke's id-less read
    std::string r = run(duke, "query_valid_cbs", R"({"action":"query_valid_cbs"})");
    row("b01_duke_idless_read_is_the_duke", g_seen_set && g_seen.id == DUKE, show(g_seen) + " " + r);

    // the duke's id-less write
    r = run(duke, "queue_intent", R"({"action":"queue_intent","interaction_type":"x"})");
    row("b02_duke_idless_write_is_the_duke", g_seen_set && g_seen.id == DUKE, show(g_seen) + " " + r);

    // a seat holding nothing: read refused, typed on the wire
    Session* empty_seat = make("seat-empty", true);
    r = run(empty_seat, "query_valid_cbs", R"({"action":"query_valid_cbs"})");
    row("b03_unbound_seat_read_refused", g_seen.source == ActorSource::Refused && g_seen.id == 0, show(g_seen));
    row("b04_unbound_seat_read_reply_typed",
        r.find("\"actor_refused\":\"no_actor\"") != std::string::npos
        && r.find("\"actor_rule\":\"seat_read_requires_actor\"") != std::string::npos, r);

    // a write by a session holding nothing: refused, never the king
    r = run(empty_seat, "set_ai_personality", R"({"action":"set_ai_personality","boldness":1})");
    row("b05_unbound_write_refused_not_the_king", g_seen.id == 0 && g_seen.source == ActorSource::Refused, show(g_seen));
    row("b06_unbound_write_reason_no_actor",
        g_seen_refusal && std::string(g_seen_refusal) == "no_actor"
        && r.find("\"reason\":\"no_actor\"") != std::string::npos
        && r.find("\"actor_rule\":\"write_requires_actor\"") != std::string::npos, r);

    // the historical layout: a session that declared NO seat keeps the mirror on a read
    Session* legacy = make("legacy-tool", false);
    r = run(legacy, "query_valid_cbs", R"({"action":"query_valid_cbs"})");
    row("b07_no_seat_read_keeps_historical_chain", g_seen.id == KING && g_seen.source == ActorSource::GlobalMirror,
        show(g_seen));
    row("b08_no_seat_read_reply_unannotated", r.find("actor_refused") == std::string::npos, r);

    // a named actor is untouched (the ownership gate vets a write's id as before)
    r = run(duke, "query_valid_cbs", R"({"action":"query_valid_cbs","actor_id":4242})");
    row("b09_named_actor_untouched", g_seen.id == 4242 && g_seen.source == ActorSource::Explicit, show(g_seen));

    // a handler that fails with no reply still answers the typed refusal: the
    // exact wire shape ck3agent's unbound_actor_block reads as no_ruler_bound
    // (test_actor_refusal_wire.py pins the server half on this string)
    r.clear();
    {
        ClientContext ctx; ctx.session = empty_seat; ClientContextGuard guard(&ctx);
        dispatch_action_with_response("query_character", R"({"action":"query_character"})", &r);
    }
    row("b11_unbound_seat_query_character_typed_empty_reply",
        r == R"({"success":false,"action":"query_character","actor_refused":"no_actor","actor_rule":"seat_read_requires_actor"})", r);

    // Through the real dispatch and the real resolve_actor /
    // resolve_character / unread_actor_key. The live defect: the
    // duke's session sent set_council_task naming the KING under character_id;
    // the verb read actor_id and wrote on the duke. The ownership gate vets the
    // named id first (owned_by_other), so the refusal is seen with the session's
    // own ruler named under the wrong key, exactly the shape C2's native write had.
    const char* kKeyReply =
        R"({"success":false,"action":"set_council_task","reason":"actor_key_not_read",)"
        R"("actor_refused":"actor_key_not_read","actor_rule":"write_names_actor_under_unread_key",)"
        R"("actor_key_read":"actor_id","actor_key_got":"character_id"})";
    r = run(duke, "set_council_task", R"({"action":"set_council_task","character_id":7002,"task":"t"})");
    row("b12_write_actor_under_unread_key_refused_nothing_written", !g_council_wrote
        && g_seen_refusal && std::string(g_seen_refusal) == "actor_key_not_read", r);
    row("b13_refusal_reply_names_both_keys_exactly", r == kKeyReply, r);
    r = run(duke, "set_council_task", R"({"action":"set_council_task","character_id":7001,"task":"t"})");
    row("b14_other_seats_ruler_under_unread_key_refused_by_ownership_gate", !g_council_wrote
        && r.find("\"reason\":\"owned_by_other\"") != std::string::npos, r);
    r = run(duke, "set_council_task", R"({"action":"set_council_task","actor_id":7002,"character_id":7001,"task":"t"})");
    row("b15_named_actor_id_wins_character_id_ignored_as_before", g_council_wrote && !g_seen_refusal, r);
    r = run(duke, "set_council_task", R"({"action":"set_council_task","task":"t"})");
    row("b16_no_id_key_write_is_own_primary_unchanged", g_council_wrote && !g_seen_refusal, r);
    r = run(duke, "set_council_task", R"({"action":"set_council_task","character_id":0,"task":"t"})");
    row("b17_character_id_zero_names_nobody_not_refused", g_council_wrote && !g_seen_refusal, r);
    r = run(duke, "set_ai_personality", R"({"action":"set_ai_personality","actor_id":7002,"boldness":1})");
    row("b18_reverse_pair_resolve_character_given_actor_id_refused",
        g_seen_refusal && std::string(g_seen_refusal) == "actor_key_not_read"
        && r.find("\"actor_key_read\":\"character_id\",\"actor_key_got\":\"actor_id\"") != std::string::npos, r);
    r = run(duke, "query_character", R"({"action":"query_character","actor_id":7001})");
    row("b19_a_read_with_the_other_key_is_untouched", r.find("actor_key_not_read") == std::string::npos, r);

    // outside any dispatch: the historical chain (tick hooks, telemetry)
    {
        ClientContext ctx; ctx.session = empty_seat; ClientContextGuard guard(&ctx);
        ActorPick p = resolve_actor_id(0, LegacyChain::SessionThenMirror);
        row("b10_outside_dispatch_keeps_historical_chain", p.id == KING && p.source == ActorSource::GlobalMirror, show(p));
    }
}
}  // namespace

int main() {
    part_a_pure();
    part_b_dispatch();
#ifdef CK3_ACTOR_PRE_FIX_CONTROL
    // The superseded resolver: the pure table (part A) is the same code, so it
    // passes; every dispatch row that depends on the new rule must fail.
    const std::set<std::string> kPreFixFails = {
        "b01_duke_idless_read_is_the_duke",
        "b02_duke_idless_write_is_the_duke",
        "b03_unbound_seat_read_refused",
        "b04_unbound_seat_read_reply_typed",
        "b05_unbound_write_refused_not_the_king",
        "b06_unbound_write_reason_no_actor",
        "b11_unbound_seat_query_character_typed_empty_reply",
        // The superseded resolver ignores the unread key and writes
        "b12_write_actor_under_unread_key_refused_nothing_written",
        "b13_refusal_reply_names_both_keys_exactly",
        "b18_reverse_pair_resolve_character_given_actor_id_refused",
    };
    const bool exact = failed == kPreFixFails;
    std::printf("PRE-FIX CONTROL: %zu rows failed, expected exactly %zu: %s\n",
                failed.size(), kPreFixFails.size(), exact ? "OK" : "MISMATCH");
    return exact ? 0 : 1;
#else
    std::printf("%s (%d failing)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
#endif
}
