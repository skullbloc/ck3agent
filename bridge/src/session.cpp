// session.cpp — M1 multi-agent identity + ownership layer implementation.

#include "session.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "bridge.h"
#include "action_dispatch_internal.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace bridge {

// Global state.
thread_local ClientContext* g_current_client_context = nullptr;
SessionRegistry g_session_registry CK3_EARLY_INIT;

// ============================================================================
// Session helpers
// ============================================================================

bool Session::claim(uint32_t char_id) {
    std::lock_guard<std::mutex> lock(mutex);
    claimed_character_ids.insert(char_id);
    primary_character_id = char_id;
    return true;
}

bool Session::release(uint32_t char_id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = claimed_character_ids.find(char_id);
    if (it == claimed_character_ids.end()) return false;
    claimed_character_ids.erase(it);
    if (primary_character_id == char_id) {
        primary_character_id = claimed_character_ids.empty()
                                   ? 0
                                   : *claimed_character_ids.begin();
    }
    return true;
}

bool Session::set_primary(uint32_t char_id) {
    std::lock_guard<std::mutex> lock(mutex);
    if (claimed_character_ids.find(char_id) == claimed_character_ids.end())
        return false;
    primary_character_id = char_id;
    return true;
}

void Session::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    primary_character_id = 0;
    heir_character_id = 0;
    claimed_character_ids.clear();
}

std::vector<uint32_t> Session::release_all() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<uint32_t> out(claimed_character_ids.begin(),
                                 claimed_character_ids.end());
    primary_character_id = 0;
    heir_character_id = 0;
    claimed_character_ids.clear();
    return out;
}

// Full JSON string escaping.  The old inline escape handled only '"' and '\\',
// which was survivable while Session::to_json had no caller — ck-6kvq.11 gave it
// one (get_bridge_state publishes the session array), and client_name is taken
// VERBATIM from the client's handshake frame.  A name containing a newline or a
// control byte would then emit invalid JSON in a reply every client parses,
// including the observer meter lane.
static void append_json_string(std::string& out, const std::string& in) {
    out += '"';
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

static void append_char_id_array(std::string& out,
                                 const std::set<uint32_t>& ids) {
    out += '[';
    bool first = true;
    for (uint32_t id : ids) {
        if (!first) out += ',';
        first = false;
        out += std::to_string(id);
    }
    out += ']';
}

std::string Session::to_json() const {
    std::string json = "{";
    json += "\"session_id\":";
    append_json_string(json, session_id);
    json += ",\"client_name\":";
    append_json_string(json, client_name);
    json += ",\"mode\":\"";
    json += session_mode_to_string(mode);
    json += "\",\"created_at_ms\":";
    json += std::to_string(created_at_ms);
    // claim_seq makes the global-primary choice auditable from get_bridge_state.
    json += ",\"claim_seq\":";
    json += std::to_string(claim_seq);
    // ck-6kvq.5: a detached session is invisible from the socket layer (its slot
    // is gone) but very much alive here, so get_bridge_state is the only place an
    // operator can see one.  The resume TOKEN is deliberately NOT published: it
    // is an authorisation over a claim set, and get_bridge_state is broadcast.
    json += ",\"session_state\":\"";
    json += session_state_to_string(state);
    json += "\",\"state_since_ms\":";
    json += std::to_string(state_since_ms);
    json += ",\"world\":";
    json += world.to_json();

    std::lock_guard<std::mutex> lock(mutex);
    json += ",\"primary_character_id\":";
    json += std::to_string(primary_character_id);
    json += ",\"heir_character_id\":";
    json += std::to_string(heir_character_id);
    json += ",\"claimed_character_ids\":";
    append_char_id_array(json, claimed_character_ids);
    json += "}";
    return json;
}

// ============================================================================
// SessionRegistry
// ============================================================================

Session* SessionRegistry::create(const std::string& client_name, Session::Mode mode) {
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    std::string sid = generate_session_id();
    std::string name = client_name.size() > MAX_CLIENT_NAME
                           ? client_name.substr(0, MAX_CLIENT_NAME)
                           : client_name;
    std::string token = generate_resume_token();
    std::lock_guard<std::mutex> lock(mutex_);
    auto up = std::make_unique<Session>(sid, std::move(name), mode, now);
    Session* ptr = up.get();
    // ck-6kvq.5: the resume credential is minted HERE, by the server, and handed
    // out in handshake_ack.  See session.h for why it is not the client's to
    // choose.
    ptr->resume_token    = token;
    ptr->state           = Session::State::ATTACHED;
    ptr->state_since_ms  = now;
    resume_tokens_[token] = sid;
    sessions_[sid] = std::move(up);
    return ptr;
}

Session* SessionRegistry::find(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    return it != sessions_.end() ? it->second.get() : nullptr;
}

bool SessionRegistry::destroy(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    {
        std::lock_guard<std::mutex> slock(it->second->mutex);
        for (uint32_t cid : it->second->claimed_character_ids) {
            ownership_.erase(cid);
        }
    }
    // A destroyed session gives up its claim-order slot AND any manifest binding.
    // The manifest CHARACTER designation survives: if the episode's designated
    // character is claimed again later, the designation re-binds to the new owner.
    if (manifest_primary_session_ == session_id) manifest_primary_session_.clear();
    // A destroyed session's resume token must die with it, or a reconnect would
    // be refused by luck (a dangling lookup) rather than by rule — and a reused
    // token could land on a different session.
    resume_tokens_.erase(it->second->resume_token);
    sessions_.erase(it);
    rebuild_legacy_mirrors_locked();
    return true;
}

Session* SessionRegistry::owner_of(uint32_t char_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ownership_.find(char_id);
    if (it == ownership_.end()) return nullptr;
    auto sit = sessions_.find(it->second);
    return sit != sessions_.end() ? sit->second.get() : nullptr;
}

bool SessionRegistry::claim_character(Session* session, uint32_t char_id,
                                      std::string* reason_out) {
    if (!session) {
        if (reason_out) *reason_out = "no_session";
        return false;
    }
    if (char_id == 0 || char_id == 0xFFFFFFFFu) {
        if (reason_out) *reason_out = "invalid_character";
        return false;
    }
    // An observe session holds no control state at all — including claims.  This
    // was previously only enforced at the dispatch choke points; enforcing it in
    // the registry keeps the ownership table exactly equal to the derived
    // control watchlist, which the divergence invariant below depends on.
    if (session->is_observe()) {
        if (reason_out) *reason_out = "observe_mode";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ownership_.find(char_id);
    if (it != ownership_.end()) {
        if (it->second == session->session_id) {
            {
                std::lock_guard<std::mutex> slock(session->mutex);
                session->claimed_character_ids.insert(char_id);
                session->primary_character_id = char_id;
            }
            note_session_claim_locked(session);
            rebuild_legacy_mirrors_locked();
            return true;
        }
        if (reason_out) *reason_out = "owned_by_other";
        return false;
    }

    // ck-6kvq.11 / design §1.5: the legacy watchlist (g_controlled_chars) is
    // DERIVED from this table, so the cap must be enforced HERE.  The old code
    // let the claim succeed and then silently skipped the watchlist add in
    // dispatch_router.cpp, leaving the two structures disagreeing.  Refusing the
    // claim keeps them identical by construction.  Observe sessions hold no
    // claims and so never count against the cap.
    if (session->is_control()) {
        // ck-bzcw: the shared counter, not a second copy of it.
        const size_t controlled = controlled_claim_count_locked();
        if (controlled >= MAX_CONTROLLED_WATCHLIST) {
            if (reason_out) *reason_out = "watchlist_full";
            if (g_logfile)
                fprintf(g_logfile,
                        "[CK3-Bridge] SESSION: claim of %u refused — controlled "
                        "watchlist full (%zu/%zu)\n",
                        char_id, controlled, MAX_CONTROLLED_WATCHLIST);
            return false;
        }
    }

    ownership_[char_id] = session->session_id;
    {
        std::lock_guard<std::mutex> slock(session->mutex);
        session->claimed_character_ids.insert(char_id);
        session->primary_character_id = char_id;
    }
    note_session_claim_locked(session);
    rebuild_legacy_mirrors_locked();
    // ck-jg2b: SAY THE CAP AND THE COUNT ON EVERY CLAIM.
    //
    // MAX_CONTROLLED_WATCHLIST is 10 and it is counted GLOBALLY, summed across
    // every control session, while the claims that consume it are per-session
    // and are only returned by an explicit release. Six seats pre-bound at
    // handshake spend six slots before the first turn.
    //
    // The coupling is real and it is invisible: the seat that gets refused is
    // not the seat that consumed the slot. diag25 reached the cap and it was
    // discovered from a roster size in an unrelated log line. Printing both
    // numbers on every claim makes the approach to the cap readable instead of
    // reconstructable.
    if (g_logfile) {
        // ck-bzcw: the shared counter here too. ck-jg2b's census line added a
        // THIRD copy of this loop, and ck-bzcw's own row caught it — which is
        // what a row that counts implementations is for.
        const size_t total = controlled_claim_count_locked();
        // ck-zgh4's baseline caught two hazards in the first spelling of this
        // line, and both are about the WITNESS rather than the log:
        //
        //   1. It read "watchlist %zu/%zu", ONE WORD from the shipped
        //      "watchlist full (%zu/%zu)" at :271. Nobody normalising either
        //      spelling toward the other would think they were breaking an
        //      instrument, and the witness would quietly stop discriminating.
        //   2. It contained an EM-DASH. strings(1) is not transparent there —
        //      it splits the C string at the \xe2\x80\x94 bytes — so any witness
        //      SPANNING one reads 0 whether or not it is present. A false PASS,
        //      in the direction that lets a missing change look landed.
        //
        // So: one unique token, no em-dash, no substring-neighbour of any
        // shipped line. `claim_watchlist_census` and `scope=global` cannot be
        // satisfied by anything that exists today.
        fprintf(g_logfile,
                "[CK3-Bridge] SESSION: claim_watchlist_census char=%u "
                "session=%s held=%zu cap=%zu scope=global\n",
                char_id, session->session_id.c_str(), total,
                MAX_CONTROLLED_WATCHLIST);
        fflush(g_logfile);
    }
    return true;
}

// ck-swap: MOVE a claim from one character to another, ATOMICALLY.
//
// WHY A PRIMITIVE AND NOT TWO CALLS. The `session(control)` tool promises to
// "switch which ruler you control after a succession" and the handler only
// ADDS, so a seat accumulated characters for twenty years and the global
// watchlist saturated — 2578 refusals across ~550 ticks with the control
// surface closed to every seat.
//
// The obvious repair, reordering two IPC calls, CANNOT WORK, and the reason is
// the whole justification for this function:
//
//   claim-then-release   the CLAIM is refused at the cap, so the ordering is
//                        unreachable exactly when it matters (ck-zgh4 recovered
//                        the sequence: two watchlist_full refusals, then a
//                        release, then the claim succeeding)
//   release-then-claim   empties the set, note_session_release_locked zeroes
//                        claim_seq, the re-claim takes a new slot from the back
//                        -> claim_seq MOVES and the succession is rejected
//
// There is no third order. So it is one step under one lock: the set is never
// empty, claim_seq is never touched, and the global count is NET ZERO so the
// cap has nothing to refuse.
//
// A SWAP IS NOT A BACK DOOR PAST THE CAP. If `from` is not actually held by
// this session the operation is a net +1, which is a CLAIM and not a swap, so
// it is delegated to claim_character where the cap is enforced. Otherwise this
// would be the cap bypass the fast path at the top of claim_character can
// already reach whenever ownership_ and a session's claim set disagree.
bool SessionRegistry::swap_character(Session* session, uint32_t from,
                                     uint32_t to, std::string* reason_out) {
    if (!session) {
        if (reason_out) *reason_out = "no_session";
        return false;
    }
    if (to == 0 || to == 0xFFFFFFFFu) {
        if (reason_out) *reason_out = "invalid_character";
        return false;
    }
    if (session->is_observe()) {
        if (reason_out) *reason_out = "observe_mode";
        return false;
    }
    if (from == to) return true;            // nothing to move

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // `to` owned elsewhere is refused exactly as claim_character refuses it.
        // A seat taking another seat's character is the one thing the arm design
        // forbids, and a swap must not become the exception.
        auto it_to = ownership_.find(to);
        if (it_to != ownership_.end() && it_to->second != session->session_id) {
            if (reason_out) *reason_out = "owned_by_other";
            return false;
        }

        auto it_from = ownership_.find(from);
        const bool we_hold_from =
            (from != 0 && it_from != ownership_.end() &&
             it_from->second == session->session_id);

        if (we_hold_from) {
            // ── THE NET-ZERO PATH. One slot out, one slot in, under one lock.
            ownership_.erase(it_from);
            ownership_[to] = session->session_id;
            {
                std::lock_guard<std::mutex> slock(session->mutex);
                // BOTH STRUCTURES, TOGETHER. Updating one and not the other is
                // precisely what makes claim_character's fast path reachable,
                // and that path inserts and returns true with NO cap check.
                session->claimed_character_ids.erase(from);
                session->claimed_character_ids.insert(to);
                session->primary_character_id = to;
                // DELIBERATELY NOT setting last_steered_character_id. That field
                // exists so a session which released its own CORPSE can still be
                // handed an heir (ck-7rk6); a swap is a deliberate switch away
                // from a character the session chose to stop steering, and
                // ck-7rk6's own comment says such a session must not be handed
                // one. The handoff only consults it when primary is 0, which a
                // swap never leaves behind.
            }
            // claim_seq is NOT touched: the set was non-empty before and after,
            // so neither note_session_claim_locked's empty->held assignment nor
            // note_session_release_locked's held->empty reset applies. That is
            // the property §7.3i reads.
            note_session_claim_locked(session);   // manifest binding only
            rebuild_legacy_mirrors_locked();
            return true;
        }
    }

    // NOT A SWAP — a net +1. The cap must see it, so take the claim path.
    return claim_character(session, to, reason_out);
}

bool SessionRegistry::release_character(Session* session, uint32_t char_id,
                                       ReleaseSubjectState state_at_release) {
    if (!session) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = ownership_.find(char_id);
    if (it == ownership_.end() || it->second != session->session_id) return false;
    ownership_.erase(it);
    {
        std::lock_guard<std::mutex> slock(session->mutex);
        session->claimed_character_ids.erase(char_id);
        if (session->primary_character_id == char_id) {
            // ck-7rk6: remember what we were steering BEFORE zeroing it, so a
            // release cannot blind the succession handoff. Set only on the way
            // out of primary, never cleared here.
            //: ck-qv51: AN UNDONE CLAIM IS NOT A RELEASE, so it records nothing.
            //:
            //: The roster-divergence rollback in do_set_controlled_character calls
            //: release_character to undo a claim the seat never steered. Recording it
            //: here overwrote a GENUINE earlier last_steered_character_id — a field
            //: set only on the way out of primary and never cleared — so a later real
            //: succession could be blinded, returning 0 for a seat that did have a
            //: subject. The primary below still updates, because the claim really is
            //: gone; what must not happen is a release being remembered that did not
            //: occur.
            if (state_at_release != ReleaseSubjectState::UndoneClaim) {
                session->last_steered_character_id = char_id;
                //: ck-7yrp: and WHAT IT WAS at this moment, recorded beside it. The id
                //: alone made a deliberate drop of a living character
                //: indistinguishable from a corpse release as soon as the character
                //: later died. Written as a PAIR with the id above, under this one
                //: mutex and this one guard, which is what bounds ck-qv51's severity
                //: at "seat stays empty" rather than "seat seized wrongly".
                session->last_release_state = state_at_release;
            }
            session->primary_character_id = session->claimed_character_ids.empty()
                                                  ? 0
                                                  : *session->claimed_character_ids.begin();
        }
    }
    note_session_release_locked(session);
    rebuild_legacy_mirrors_locked();
    return true;
}

// ----------------------------------------------------------------------------
// Deterministic global primary (ck-6kvq.11, design §1.4 defect 2 / §7 item 2)
// ----------------------------------------------------------------------------

void SessionRegistry::note_session_claim_locked(Session* session) {
    // Caller holds mutex_.  Assign a claim-order slot on the transition from
    // "holds nothing" to "holds something".  A session that keeps at least one
    // claim keeps its slot, so a succession handoff (claim heir, then release
    // the corpse) does not move the global primary.
    //: ck-j6lk: FIRST, and unconditionally — before the claim_seq branch, because
    //: claim_seq only moves on the empty->held transition and this must count every
    //: claim including a swap, where the set is non-empty on both sides. This is
    //: the one write; nothing ever clears it.
    session->claims_ever.fetch_add(1, std::memory_order_relaxed);

    if (session->claim_seq == 0) session->claim_seq = next_claim_seq_++;

    // Bind a pending manifest designation to whichever session claimed the
    // designated character.
    if (manifest_primary_char_ != 0 && manifest_primary_session_.empty()) {
        auto it = ownership_.find(manifest_primary_char_);
        if (it != ownership_.end()) manifest_primary_session_ = it->second;
    }
}

void SessionRegistry::note_session_release_locked(Session* session) {
    // Caller holds mutex_.  A session that has given up its last claim also
    // gives up its claim-order slot; re-claiming later puts it at the BACK of
    // the order rather than letting it silently steal the primary back.
    bool empty = false;
    {
        std::lock_guard<std::mutex> slock(session->mutex);
        empty = session->claimed_character_ids.empty();
    }
    if (empty) session->claim_seq = 0;
}

Session* SessionRegistry::resolve_manifest_session_locked() {
    // Caller holds mutex_.
    if (manifest_primary_char_ == 0) return nullptr;

    // Preferred: the session that currently owns the designated character.
    auto own = ownership_.find(manifest_primary_char_);
    if (own != ownership_.end()) {
        auto sit = sessions_.find(own->second);
        if (sit != sessions_.end() && sit->second->is_control()) {
            manifest_primary_session_ = own->second;
            return sit->second.get();
        }
    }

    // Fallback: the designation stays bound to the session it was bound to, so
    // a succession (designated king dies, his session claims the heir and
    // releases the corpse) keeps the global primary on the same agent.
    if (!manifest_primary_session_.empty()) {
        auto sit = sessions_.find(manifest_primary_session_);
        if (sit != sessions_.end() && sit->second->is_control()) {
            return sit->second.get();
        }
        manifest_primary_session_.clear();
    }
    return nullptr;
}

void SessionRegistry::set_manifest_primary(uint32_t char_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    manifest_primary_char_ = char_id;
    manifest_primary_session_.clear();
    if (char_id != 0) {
        auto it = ownership_.find(char_id);
        if (it != ownership_.end()) manifest_primary_session_ = it->second;
    }
    rebuild_legacy_mirrors_locked();
}

uint32_t SessionRegistry::manifest_primary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return manifest_primary_char_;
}

std::string SessionRegistry::manifest_primary_session() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return manifest_primary_session_;
}

bool SessionRegistry::ownership_divergence(std::string* detail) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (detail) *detail = divergence_;
    return !divergence_.empty();
}

// ck-bzcw: ONE COMPUTATION, THREE READERS.
//
// This function and claim_character's cap check were the SAME loop written
// TWICE — and this copy had ZERO CALLERS, so the live one was the inline
// duplicate and this one had never run. Exposing occupancy to the driver from
// the dead copy would have published a number produced by code that enforces
// nothing, beside a cap enforced by a different copy; if they ever drifted the
// covariate would disagree with the thing that refuses, invisibly.
//
// So the loop lives here once. claim_character calls the _locked form (it
// already holds mutex_), get_bridge_state calls the public one, and the number
// the driver records is BY CONSTRUCTION the number that refuses.
size_t SessionRegistry::controlled_claim_count_locked() const {
    size_t n = 0;
    for (const auto& kv : sessions_) {
        const Session& s = *kv.second;
        //: DETACHED and LOST sessions STILL HOLD CLAIMS (session.h:148) and are
        //: deliberately counted: a seat that reconnects without resuming holds
        //: two slots, and the cap must see both or it would refuse a claim the
        //: census said there was room for.
        if (s.is_observe()) continue;
        std::lock_guard<std::mutex> slock(s.mutex);
        n += s.claimed_character_ids.size();
    }
    return n;
}

size_t SessionRegistry::controlled_claim_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return controlled_claim_count_locked();
}

void SessionRegistry::rebuild_legacy_mirrors() {
    std::lock_guard<std::mutex> lock(mutex_);
    rebuild_legacy_mirrors_locked();
}

void SessionRegistry::rebuild_legacy_mirrors_locked() {
    // Caller must already hold mutex_.
    std::lock_guard<std::mutex> mirror_lock(g_controlled_mutex);

    // Pass 1 — collect the watchlist and find the claim-order winner.  The old
    // code took "the first control session with a nonzero primary" as seen while
    // walking sessions_, an unordered_map: at N>1 that is HASH order, not
    // creation order, so the global primary moved on every rehandshake.  We
    // instead select by claim_seq, which is explicit and stable.
    const Session* by_claim_order = nullptr;
    uint64_t best_seq = std::numeric_limits<uint64_t>::max();
    std::vector<uint32_t> chars;
    for (const auto& kv : sessions_) {
        const Session& s = *kv.second;
        if (s.is_observe()) continue;
        std::lock_guard<std::mutex> slock(s.mutex);
        if (s.primary_character_id != 0) {
            // A session with no claim-order slot (primary set directly, without
            // going through the registry) sorts LAST but is still eligible, so
            // the mirror never blanks just because bookkeeping was bypassed.
            uint64_t seq = s.claim_seq != 0
                               ? s.claim_seq
                               : std::numeric_limits<uint64_t>::max();
            // Tiebreak on session_id (timestamp + monotonic counter) so even the
            // degenerate all-bypassed case never falls back to hash order.
            if (!by_claim_order || seq < best_seq ||
                (seq == best_seq && s.session_id < by_claim_order->session_id)) {
                by_claim_order = &s;
                best_seq = seq;
            }
        }
        for (uint32_t cid : s.claimed_character_ids) {
            chars.push_back(cid);
        }
    }

    // Pass 2 — the episode manifest wins over claim order when it resolves.
    const Session* chosen = resolve_manifest_session_locked();
    if (!chosen) chosen = by_claim_order;

    uint32_t primary = 0;
    uint32_t heir = 0;
    if (chosen) {
        std::lock_guard<std::mutex> slock(chosen->mutex);
        primary = chosen->primary_character_id;
        heir = chosen->heir_character_id;
    }
    // A manifest-designated session that momentarily holds nothing must not
    // blank the global primary while another control session still holds one.
    if (primary == 0 && by_claim_order && chosen != by_claim_order) {
        std::lock_guard<std::mutex> slock(by_claim_order->mutex);
        primary = by_claim_order->primary_character_id;
        heir = by_claim_order->heir_character_id;
    }

    std::sort(chars.begin(), chars.end());

    // --- Invariant check: the mirrors must AGREE with the ownership table. ---
    // A violation is an error, not a log line (design §1.5): it is latched here
    // and surfaced through ownership_divergence() so callers can fail loudly.
    divergence_.clear();
    if (chars.size() != ownership_.size()) {
        divergence_ = "watchlist/ownership size mismatch: watchlist=" +
                      std::to_string(chars.size()) +
                      " ownership=" + std::to_string(ownership_.size());
    } else if (std::adjacent_find(chars.begin(), chars.end()) != chars.end()) {
        divergence_ = "watchlist contains a duplicate character";
    } else if (chars.size() > MAX_CONTROLLED_WATCHLIST) {
        divergence_ = "watchlist over cap: " + std::to_string(chars.size()) +
                      "/" + std::to_string(MAX_CONTROLLED_WATCHLIST);
    } else if (primary != 0 &&
               !std::binary_search(chars.begin(), chars.end(), primary)) {
        divergence_ = "global primary " + std::to_string(primary) +
                      " is not in the controlled watchlist";
    }
    if (!divergence_.empty() && g_logfile) {
        fprintf(g_logfile, "[CK3-Bridge] OWNERSHIP_DIVERGENCE: %s\n",
                divergence_.c_str());
        fflush(g_logfile);
    }

    g_controlled_primary.store(primary, std::memory_order_relaxed);
    g_controlled_heir.store(heir, std::memory_order_relaxed);
    g_controlled_chars = std::move(chars);

    // g_agent_mode is true whenever any control session holds a primary.
    g_agent_mode.store(primary != 0, std::memory_order_relaxed);
}

std::string SessionRegistry::sessions_json() const {
    // ck-6kvq.11: ORDERED, not hash-ordered. get_bridge_state now publishes this
    // array, and emitting it in unordered_map order would re-export the exact
    // nondeterminism this bead removed from the global primary — a client
    // diffing two snapshots would see the roster "change" on every rehash.
    //
    // The order is the same rule that elects the global primary: lowest nonzero
    // claim_seq first (a session holding nothing sorts last), session_id as the
    // tiebreak. So when claim order decides, the head of this array IS the
    // global primary's session, and the choice is auditable from the wire.
    std::vector<const Session*> ordered;
    std::string json = "[";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ordered.reserve(sessions_.size());
        for (const auto& kv : sessions_) ordered.push_back(kv.second.get());
        std::sort(ordered.begin(), ordered.end(),
                  [](const Session* a, const Session* b) {
                      uint64_t sa = a->claim_seq != 0
                                        ? a->claim_seq
                                        : std::numeric_limits<uint64_t>::max();
                      uint64_t sb = b->claim_seq != 0
                                        ? b->claim_seq
                                        : std::numeric_limits<uint64_t>::max();
                      if (sa != sb) return sa < sb;
                      return a->session_id < b->session_id;
                  });
        bool first = true;
        for (const Session* s : ordered) {
            if (!first) json += ',';
            first = false;
            json += s->to_json();
        }
    }
    json += "]";
    return json;
}

std::vector<std::string> SessionRegistry::ids() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(sessions_.size());
    for (const auto& kv : sessions_) out.push_back(kv.first);
    return out;
}

// ============================================================================
// DETACH / RESUME (ck-6kvq.5, design §6.2 / §1.4 defect 1 / §7 item 1)
// ============================================================================
//
// ck-6kvq.10 stopped the destroy-on-drop under `episode_pins_claims()`.  This is
// the other half: the session it left alive gets a STATE, a way back in, and a
// bound on how long it may be gone.
//
// Read `session.h` for the contracts.  The invariant that governs every line
// below: nothing here re-enables `ai_ctrl+0x28`, and nothing here releases a
// claim except on the one path where the claims no longer refer to the
// characters they were taken for (a refuted world — ck-9fc3).

void SessionRegistry::set_steering_purge_hook(SteeringPurgeHook hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    steering_purge_hook_ = std::move(hook);
}

void SessionRegistry::attach_to_slot(Session* session, int slot_idx,
                                     uint32_t slot_gen, uint64_t now_ms) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(mutex_);
    session->bound_slot_idx = slot_idx;
    session->bound_slot_gen = slot_gen;
    if (session->state != Session::State::ATTACHED) {
        session->state = Session::State::ATTACHED;
        session->state_since_ms = now_ms;
    }
}

void SessionRegistry::set_session_world(Session* session,
                                        const WorldIdentity& world,
                                        uint64_t epoch) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!world.empty()) session->world = world;
    if (epoch) session->attach_epoch = epoch;
}

bool SessionRegistry::detach_from_slot(const std::string& session_id,
                                       int slot_idx, uint32_t slot_gen,
                                       uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;
    Session* s = it->second.get();

    // THE STALE-GENERATION GUARD.  A close may only tear down the binding it
    // actually saw.  If the client has already reconnected, the session is bound
    // to a different slot (or to a newer generation of the same slot index,
    // after that slot was recycled), and this close is a ghost from a socket
    // that is already dead.  Honouring it would take a LIVE session's claims
    // offline — the exact failure §6.2 names, and the one a late peer-pid reap
    // or keepalive sweep produces in practice.
    const bool forced = (slot_idx < 0);
    if (!forced) {
        if (s->bound_slot_idx != slot_idx) return false;
        if (s->bound_slot_gen != slot_gen) return false;
    }

    if (s->state == Session::State::ATTACHED) {
        s->state = Session::State::DETACHED;
        s->state_since_ms = now_ms;
    }
    // Unbind the slot and KEEP EVERYTHING ELSE: claimed_character_ids,
    // ownership_, primary, heir, claim_seq, the manifest binding and the resume
    // token.  rebuild_legacy_mirrors is deliberately NOT called — nothing about
    // the ownership table changed, and rebuilding would only risk moving the
    // global primary on a socket event.
    s->bound_slot_idx = -1;
    s->bound_slot_gen = 0;
    return true;
}

// ----------------------------------------------------------------------------
// The resume identity clause
// ----------------------------------------------------------------------------
// Deliberately a free function taking everything it needs: the whole rule is one
// readable expression, and a test can drive it without a registry.
//
// ck-9fc3's judge_same_save() is THE rule for "is this the same save"; this adds
// only the world-epoch shortcut (the world never changed, so no identity is
// needed) and §6.2's discard-on-unvouched-change bias.
namespace {

enum class ResumeVerdict { ALLOW, DISCARD };

// CK3_RESUME_NO_IDENTITY_CHECK builds the NEGATIVE CONTROL: the clause always
// allows, and the ck-9fc3 symptom comes straight back (a session keeps claims —
// and therefore steering — over characters in a world it never saw).  An
// acceptance that cannot fail is not evidence.
#ifdef CK3_RESUME_NO_IDENTITY_CHECK
constexpr bool kIdentityCheckEnabled = false;
#else
constexpr bool kIdentityCheckEnabled = true;
#endif

ResumeVerdict resume_identity_verdict(const WorldIdentity& recorded,
                                      const WorldIdentity& presented,
                                      uint64_t attach_epoch,
                                      uint64_t current_epoch,
                                      const char** why) {
    const char* dummy = nullptr;
    if (!why) why = &dummy;

    if (!kIdentityCheckEnabled) {
        *why = "identity_check_disabled_NEGATIVE_CONTROL";
        return ResumeVerdict::ALLOW;
    }

    // (1) THE WORLD NEVER CHANGED.  ck-hxh7's epoch is monotonic and bumps only
    // on a detected load, so equal epochs mean the very same world object the
    // claims were taken in.  There is nothing for an identity to disagree about,
    // and a socket drop is not a world event.  This is the common case: a
    // subscription-cap disconnect and a reconnect nine game-days later.
    if (attach_epoch != 0 && current_epoch != 0 && attach_epoch == current_epoch) {
        *why = "same_world_epoch";
        return ResumeVerdict::ALLOW;
    }

    // (2) THE EPOCH IS UNAVAILABLE.  0 is the seam's "the detector never spoke"
    // sentinel (world_epoch() starts at 1 and only ever increments, so a live
    // world never reads 0).  With no evidence of a change and no declaration to
    // weigh, refusing would break resume on every host where ck-hxh7 is not
    // wired — so abstain, and leave the steering question to ck-9fc3, which
    // keeps the locks quarantined until someone declares.
    if (attach_epoch == 0 || current_epoch == 0) {
        if (!recorded.empty() && !presented.empty()) {
            std::string ignored;
            if (judge_same_save(recorded, presented, 0, false, &ignored) == SameSave::YES) {
                *why = "declared_same_save";
                return ResumeVerdict::ALLOW;
            }
            *why = "declared_different_save";
            return ResumeVerdict::DISCARD;
        }
        *why = "world_epoch_unavailable_abstain";
        return ResumeVerdict::ALLOW;
    }

    // (3) THE WORLD CHANGED.  From here the bias inverts relative to ck-hxh7:
    // there a false positive costs one cache recompute, here a false negative
    // rewrites a stranger's personality 3x/tick for the rest of the episode.
    // Only a declaration can say YES.
    if (recorded.empty()) {
        // Nobody ever vouched for the world the claims were taken in, so nothing
        // can prove they belong here.  The world demonstrably changed.  Discard.
        *why = "world_changed_no_recorded_identity";
        return ResumeVerdict::DISCARD;
    }
    if (presented.empty()) {
        // The client resumed without vouching for the world now loaded.  It may
        // still declare separately (reconcile_declared_world is where that
        // answer lands) — but it has not yet, and the world changed, so there is
        // no affirmation to rely on.
        *why = "world_changed_no_declared_identity";
        return ResumeVerdict::DISCARD;
    }
    std::string detail;
    const SameSave v = judge_same_save(recorded, presented, 0, false, &detail);
    if (v == SameSave::YES) {
        *why = "declared_same_save_after_reload";
        return ResumeVerdict::ALLOW;
    }
    *why = same_save_name(v);
    return ResumeVerdict::DISCARD;
}

}  // namespace

void SessionRegistry::discard_session_locked(Session* session,
                                             std::vector<uint32_t>* out_claims) {
    if (!session) return;
    std::vector<uint32_t> claims;
    {
        std::lock_guard<std::mutex> slock(session->mutex);
        claims.assign(session->claimed_character_ids.begin(),
                      session->claimed_character_ids.end());
        session->claimed_character_ids.clear();
        session->primary_character_id = 0;
        session->heir_character_id    = 0;
        session->primary_title_id     = 0;
    }
    for (uint32_t cid : claims) ownership_.erase(cid);
    session->claim_seq = 0;

    // COPY the id before the erase: unordered_map::erase(key) takes the key by
    // reference, and session_id lives INSIDE the object the erase destroys
    // (the ck-xqol lesson, applied here before it can bite).
    const std::string dead_sid = session->session_id;
    if (manifest_primary_session_ == dead_sid) manifest_primary_session_.clear();
    resume_tokens_.erase(session->resume_token);
    sessions_.erase(dead_sid);
    rebuild_legacy_mirrors_locked();

    if (out_claims) *out_claims = std::move(claims);
}

SessionRegistry::ResumeOutcome SessionRegistry::resume_by_token(
        const std::string& token, const WorldIdentity& presented,
        uint64_t current_epoch, int slot_idx, uint32_t slot_gen,
        uint64_t now_ms) {
    ResumeOutcome out;
    if (token.empty()) {
        out.status = "unknown_token";
        out.identity_reason = "no_token_presented";
        return out;
    }

    SteeringPurgeHook hook;
    std::string purge_sid;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto tit = resume_tokens_.find(token);
        if (tit == resume_tokens_.end()) {
            out.status = "unknown_token";
            return out;
        }
        auto sit = sessions_.find(tit->second);
        if (sit == sessions_.end()) {
            // The token outlived its session.  Reap it so the next presentation
            // is refused by rule rather than by a dangling lookup.
            resume_tokens_.erase(tit);
            out.status = "unknown_token";
            return out;
        }
        Session* s = sit->second.get();

        // A session still ATTACHED to a live slot is NOT resumable.  Two clients
        // holding one session's claims is a split brain, and the second one is
        // far more likely to be a confused retry than a genuine takeover.  LOST
        // IS resumable: the agent came back late, its claims were never
        // released, so there is something real to come back to — and ck-6kvq.1
        // has already reported the expiry either way.
        if (s->state == Session::State::ATTACHED && s->bound_slot_idx >= 0) {
            out.session = s;
            out.status  = "session_attached";
            out.identity_reason = "already_bound_to_a_live_slot";
            return out;
        }

        const char* why = "";
        const ResumeVerdict verdict = resume_identity_verdict(
            s->world, presented, s->attach_epoch, current_epoch, &why);
        out.identity_reason = why;

        if (verdict == ResumeVerdict::ALLOW) {
            s->bound_slot_idx = slot_idx;
            s->bound_slot_gen = slot_gen;
            s->state          = Session::State::ATTACHED;
            s->state_since_ms = now_ms;
            if (!presented.empty()) s->world = presented;
            if (current_epoch)      s->attach_epoch = current_epoch;
            // The claims never moved, so the mirrors are already right.  Rebuild
            // anyway: it is idempotent, and it re-runs the §1.5 divergence
            // invariant at exactly the moment a session's binding changed.
            rebuild_legacy_mirrors_locked();
            out.session = s;
            out.ok      = true;
            out.status  = "ok";
            return out;
        }

        // DISCARD.  The world this session was steering is gone, so its claims
        // no longer name the characters they were taken for.  This is the ONE
        // place where releasing them is correct (ck-9fc3).
        purge_sid  = s->session_id;
        out.status = "world_changed";
        discard_session_locked(s, &out.discarded_claims);
        s = nullptr;
        if (g_logfile) {
            fprintf(g_logfile,
                    "[CK3-Bridge] RESUME REFUSED session=%s reason=%s — %zu claim(s) "
                    "RELEASED and the session ERASED; the client must handshake "
                    "fresh (a claimless orphan would hold an unavailability span "
                    "to the horizon)\n",
                    purge_sid.c_str(), why, out.discarded_claims.size());
            fflush(g_logfile);
        }
        hook = steering_purge_hook_;
    }

    // Outside mutex_ so the hook may call back in (it erases per-character
    // steering state — g_personality_locks, ck-9fc3).
    if (hook) hook(purge_sid, out.discarded_claims);
    return out;
}

std::vector<std::string> SessionRegistry::reconcile_declared_world(
        const WorldIdentity& declared, uint64_t current_epoch,
        std::vector<uint32_t>* discarded_out) {
    std::vector<std::string> erased;
    std::vector<uint32_t> all_claims;
    SteeringPurgeHook hook;
    std::vector<std::pair<std::string, std::vector<uint32_t>>> to_purge;

    // A declaration that names no save affirms nothing and refutes nothing.
    if (declared.empty()) return erased;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // NOTE THE RULE IS NOT resume_identity_verdict's.  That one leads with
        // "same world epoch -> allow", which is right at the resume door (a
        // socket drop is not a world event) and WRONG here: a declaration is a
        // statement about the world NOW LOADED, so a session whose recorded
        // world names a different save is in the wrong world whether or not the
        // epoch moved.  One of the two is lying, and §6.2's bias says the
        // unvouched one loses.
        std::vector<Session*> doomed;
        std::vector<Session*> survivors;
        for (auto& kv : sessions_) {
            Session* s = kv.second.get();
            if (s->world.empty()) {            // vouched for nothing; a
                survivors.push_back(s);        // declaration can only say NO
                continue;
            }
            std::string why;
            if (judge_same_save(s->world, declared, 0, false, &why) == SameSave::YES) {
                survivors.push_back(s);
                continue;
            }
            doomed.push_back(s);
        }
        for (Session* s : doomed) {
            const std::string sid = s->session_id;
            std::vector<uint32_t> claims;
            discard_session_locked(s, &claims);
            if (g_logfile) {
                fprintf(g_logfile,
                        "[CK3-Bridge] declare_save_identity REFUTES session %s — "
                        "%zu claim(s) released, session erased\n",
                        sid.c_str(), claims.size());
                fflush(g_logfile);
            }
            all_claims.insert(all_claims.end(), claims.begin(), claims.end());
            erased.push_back(sid);
            to_purge.emplace_back(sid, std::move(claims));
        }
        // A surviving ATTACHED session is provably live in the world the harness
        // just vouched for, so record that: it is the baseline its NEXT resume
        // will be judged against, and without it a session that connected before
        // the first declaration would abstain forever.  DETACHED and LOST
        // sessions are deliberately left alone — they may have dropped before
        // this world was loaded, and stamping one would be the bridge vouching
        // on a client's behalf.
        if (current_epoch) {
            for (Session* s : survivors) {
                if (s->state != Session::State::ATTACHED) continue;
                s->world = declared;
                s->attach_epoch = current_epoch;
            }
        }
        hook = steering_purge_hook_;
    }

    if (hook)
        for (auto& pr : to_purge) hook(pr.first, pr.second);

    std::sort(erased.begin(), erased.end());
    if (discarded_out) *discarded_out = std::move(all_claims);
    return erased;
}

std::vector<std::string> SessionRegistry::sweep_detached(uint64_t now_ms,
                                                         uint64_t max_detach_ms) {
    std::vector<std::string> newly_lost;
    if (max_detach_ms == 0) return newly_lost;   // bound disabled
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : sessions_) {
        Session* s = kv.second.get();
        if (s->state != Session::State::DETACHED) continue;
        if (now_ms < s->state_since_ms) continue;              // clock went back
        if (now_ms - s->state_since_ms < max_detach_ms) continue;
        // REPORT, DO NOT RELEASE.  See session.h.
        s->state = Session::State::LOST;
        s->state_since_ms = now_ms;
        newly_lost.push_back(s->session_id);
        size_t held = 0;
        {
            std::lock_guard<std::mutex> slock(s->mutex);
            held = s->claimed_character_ids.size();
        }
        if (g_logfile) {
            // ck-bw1s: say WHICH it is.  An observe session reaching LOST is
            // state-reporting only — session_publish_detach_state() counts
            // control sessions, so an observer never produces a session_lost
            // verdict, and a log line claiming it does would send the next
            // operator hunting the wrong session.
            fprintf(g_logfile,
                    "[CK3-Bridge] SESSION %s LOST — detached beyond max_detach_s; "
                    "claims RETAINED (%zu), ai_ctrl+0x28 untouched; %s\n",
                    s->session_id.c_str(), held,
                    s->is_control()
                        ? "reported for episode abort reason=session_lost"
                        : "OBSERVE mode — NOT reported to the abort ladder "
                          "(no claims, no ledger row, no arm)");
            fflush(g_logfile);
        }
    }
    // Stable order so a caller diffing two sweeps sees no spurious churn.
    std::sort(newly_lost.begin(), newly_lost.end());
    return newly_lost;
}

std::vector<std::string> SessionRegistry::lost_sessions() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& kv : sessions_)
        if (kv.second->state == Session::State::LOST) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

// ----------------------------------------------------------------------------
// The abort ladder's detach report.  Contract documented in session.h.
// ----------------------------------------------------------------------------
namespace {
// ck-22n3: CK3_EARLY_INIT, like every other namespace-scope global in this TU.
// dll_init() runs at .init_array[1] and starts the reader, server and conductor
// threads synchronously; an unprioritised initialiser here would construct AFTER
// them, and `bridge/test/test_init_order` fails the build for exactly that.
std::mutex  g_episode_dir_mu CK3_EARLY_INIT;
std::string g_episode_dir_override CK3_EARLY_INIT;

// ck-bw1s: THE NEGATIVE CONTROL.  Built with CK3_DETACH_REPORT_COUNTS_OBSERVERS
// the report counts EVERY non-attached session — exactly the pre-fix behaviour —
// and bridge/test/acceptance_session_resume's observer lane MUST FAIL.  A
// predicate nobody has watched fail is a predicate nobody has tested.
#ifdef CK3_DETACH_REPORT_COUNTS_OBSERVERS
constexpr bool kDetachReportControlOnly = false;
#else
constexpr bool kDetachReportControlOnly = true;
#endif

// ck-bw1s: the ONE rule for "does this session's absence belong in the abort
// ladder's session_lost verdict".  A free function so the predicate has exactly
// one spelling: the release path may still DETACH an observer (get_bridge_state
// stays honest about a socket that went away), but only a CONTROL session's
// detach is EVIDENCE about the experimental condition.
bool detach_is_reportable(const Session& s) {
    if (s.state == Session::State::ATTACHED) return false;
    if (!kDetachReportControlOnly) return true;   // NEGATIVE CONTROL: pre-fix
    return s.is_control();
}
}  // namespace

void session_set_episode_dir_for_test(const char* dir) {
    std::lock_guard<std::mutex> lk(g_episode_dir_mu);
    g_episode_dir_override = (dir && dir[0]) ? dir : "";
}

void session_publish_detach_state() {
    std::string episode_dir;
    {
        std::lock_guard<std::mutex> lk(g_episode_dir_mu);
        episode_dir = g_episode_dir_override;
    }
    if (episode_dir.empty()) {
        const char* dir_env = getenv("CK3_EPISODE_DIR");
        if (dir_env && dir_env[0]) {
            episode_dir = dir_env;
        } else {
            // episode_abort.sh: EPISODE_DIR="${CK3_EPISODE_DIR:-$CK3_INSTANCE_DIR/episode}".
            // Same spelling, no third name.
            const char* inst = getenv("CK3_INSTANCE_DIR");
            if (!inst || !inst[0]) return;   // no instance dir -> no episode
            episode_dir = std::string(inst) + "/episode";
        }
    }

    // No episode directory -> no episode is running -> nothing reads this, and a
    // free-play bridge must not start creating files for it.
    struct stat st;
    if (stat(episode_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;

    const std::string state_dir = episode_dir + "/state";
    const std::string path      = state_dir + "/detached-since";

    // Earliest outstanding detach, in MONOTONIC ms.  LOST counts: that session
    // has not come back, and the rung is meant to keep firing.
    //
    // CONTROL SESSIONS ONLY (ck-bw1s).  An observe-mode session is not a
    // participant in the arm: it holds no claims, it is absent from the
    // unavailability ledger (pipeline_note_session_open skips non-control
    // sessions), and its going away changes NOTHING about the experimental
    // condition.  Counting one here lets a dropped observer abort a healthy
    // episode with reason=session_lost — a FALSE verdict, arriving through a
    // different door than the stale timestamp this contract already warns about.
    //
    // It is not hypothetical.  The meter lane is deliberately an observer
    // (ck-6kvq.7: read-only, the only reader that cannot contaminate what it
    // measures), and ck-zgh4's framing is the one worth keeping next to the
    // predicate: THE CAREFUL THING TO DO IS THE THING THAT BREAKS IT — a harness
    // author who opens a fresh observe session per probe, precisely so as not to
    // hold state, manufactures one detach event per probe.
    bool     any = false;
    uint64_t earliest_mono_ms = 0;
    g_session_registry.for_each([&](const Session& s) {
        if (!detach_is_reportable(s)) return;
        if (!any || s.state_since_ms < earliest_mono_ms) {
            earliest_mono_ms = s.state_since_ms;
            any = true;
        }
    });

    if (!any) {
        // Removed on reattach.  A stale timestamp would abort a HEALTHY episode
        // `session_lost` as soon as max_detach_s elapsed.
        ::unlink(path.c_str());
        return;
    }

    // The reader wants WALL-CLOCK epoch seconds; the registry keeps monotonic ms
    // (right for measuring an interval, useless as an absolute stamp).  Anchor
    // the interval to the current wall clock — also the only conversion that
    // stays right across an NTP step.
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    const uint64_t now_mono_ms =
        static_cast<uint64_t>(mono.tv_sec) * 1000ull + mono.tv_nsec / 1000000ull;
    const uint64_t held_ms =
        now_mono_ms > earliest_mono_ms ? now_mono_ms - earliest_mono_ms : 0;
    const long long since_epoch_s =
        static_cast<long long>(time(nullptr)) - static_cast<long long>(held_ms / 1000ull);

    ::mkdir(state_dir.c_str(), 0777);
    // Write-and-rename so the reader never sees a half-written integer: its parse
    // is unguarded bash arithmetic and a torn read would abort the run.
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "%lld\n", since_epoch_s);
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) ::unlink(tmp.c_str());
}

// ============================================================================
// Session id + mode helpers
// ============================================================================

std::string generate_session_id() {
    static std::atomic<uint64_t> counter{0};
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[64];
    snprintf(buf, sizeof(buf), "sess_%016lx_%08lx", now, n);
    return std::string(buf);
}

std::string generate_resume_token() {
    // Server-minted (session.h).  Not a secret — the socket is 0666 and any local
    // user can connect — but it must be UNGUESSABLE BY ACCIDENT, so a client
    // replaying a stale token from a previous run cannot land on some other live
    // session's claims.  Same construction as generate_session_id, with a
    // distinct prefix and its own counter namespace.
    static std::atomic<uint64_t> counter{0};
    uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[80];
    snprintf(buf, sizeof(buf), "rsm_%016llx_%08llx",
             (unsigned long long)now, (unsigned long long)n);
    return std::string(buf);
}

const char* session_state_to_string(Session::State state) {
    switch (state) {
        case Session::State::ATTACHED: return "attached";
        case Session::State::DETACHED: return "detached";
        case Session::State::LOST:     return "lost";
    }
    return "attached";
}

const char* session_mode_to_string(Session::Mode mode) {
    switch (mode) {
        case Session::Mode::CONTROL: return "control";
        case Session::Mode::OBSERVE: return "observe";
    }
    return "control";
}

Session::Mode session_mode_from_string(const char* s, Session::Mode default_mode) {
    if (!s) return default_mode;
    if (strcmp(s, "observe") == 0) return Session::Mode::OBSERVE;
    if (strcmp(s, "control") == 0) return Session::Mode::CONTROL;
    return default_mode;
}

// ============================================================================
// Dispatch helpers
// ============================================================================

bool require_dispatch_context(const char* action_type, std::string* reason) {
    (void)action_type;  // reserved for future logging / deny-list
    if (g_current_client_context) return true;
    if (reason) *reason = "no_client_context";
    return false;
}

bool action_is_readonly(const char* action_type) {
    if (!action_type || !action_type[0]) return false;

    // Queries (is_game_state_query covers these, but we duplicate the prefix
    // rule to avoid a header cycle with snapshot.h).
    static const char* kReadonly[] = {
        "get_bridge_state",
        "get_tick_info",
        "get_controlled_roster",
        "get_player_character",
        "get_ai_personality",
        "get_ai_evaluations",
        "get_offset_validation",
        "get_active_wars",
        "list_watchlist",
        "list_interactions",
        "list_decisions",
        "list_valid_decisions",
        "list_decisions_annotated",
        "who_holds_the_lock",
        // ck-6kvq.13 (R6): the budget ledger readback is a pure bridge-state
        // read — an observe-mode session (the episode harness) must be able to
        // pull it for the bundle.
        "get_budget_state",
        // ck-6kvq.12 (R4): READING the board is a read. This is what lets an
        // OBSERVE-mode session — the meter lane, and the harness draining
        // board.jsonl into the bundle — see every post while being structurally
        // unable to make one: `board_post` is absent from this table, so it is a
        // mutation, and the dispatch choke point refuses a mutation from an
        // observe session with a typed `observe_mode`.
        "board_read",
    };
    for (const char* r : kReadonly) {
        if (strcmp(action_type, r) == 0) return true;
    }
    if (strncmp(action_type, "query_", 6) == 0) return true;
    return false;
}

bool episode_observer_may_declare(Session::Mode minted_mode) {
    return minted_mode != Session::Mode::CONTROL;
}

std::string episode_observer_winner(const std::string& current,
                                    const std::string& candidate) {
    if (!current.empty()) return current;      // first declaration wins
    return candidate;
}

// ck-m3n5. ONE DECISION, so the applier has no policy of its own. The order is
// load-bearing: the MODE floor is asked FIRST, because a control session must be
// refused whether or not anybody else has declared — asking first-wins first
// would confer on an unclaimed privilege before the floor ever ran.
ObserverDecision episode_observer_decision(const std::string& current,
                                           const std::string& candidate,
                                           Session::Mode minted_mode) {
    if (!episode_observer_may_declare(minted_mode))
        return ObserverDecision::RefusedByMode;
    if (episode_observer_winner(current, candidate) != candidate)
        return ObserverDecision::RefusedByFirstWins;
    return ObserverDecision::Confer;
}

bool action_is_mutation(const char* action_type) {
    return !action_is_readonly(action_type);
}

bool validate_actor_ownership(uint32_t actor_id, const char* action_type,
                              std::string* reason) {
    Session* s = current_session();
    if (!s) return true;  // legacy path: no session context installed

    // Read-only actions don't require an actor ownership check.
    if (action_is_readonly(action_type)) return true;

    // Explicit ownership checks apply only to control sessions.
    if (s->is_observe()) {
        if (reason) *reason = "observe_mode";
        return false;
    }

    if (actor_id == 0 || actor_id == 0xFFFFFFFFu) {
        if (reason) *reason = "no_actor";
        return false;
    }

    Session* owner = g_session_registry.owner_of(actor_id);
    if (!owner) {
        if (reason) *reason = "actor_not_claimed";
        return false;
    }
    if (owner->session_id != s->session_id) {
        if (reason) *reason = "owned_by_other";
        return false;
    }
    return true;
}

} // namespace bridge
