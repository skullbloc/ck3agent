// drain_registry.cpp — ck-6kvq.19. See drain_registry.h for the bug.
//
// Engine-free ON PURPOSE. The pre-fix correlation lived inline in pipeline.cpp,
// which links the whole game-facing bridge, so it could only ever be exercised
// against a live CK3 — and every live exercise happened to send a request_id.
// Pulled out here, the hand-off/settle protocol is a host test.
//
// NEGATIVE CONTROL: compiling this TU with -DCK3_DRAIN_LEGACY_RID_KEY=1
// restores the pre-fix correlation verbatim (keyed by the client's optional,
// non-unique request_id, registered only when non-empty). Every assertion the
// fixed build passes MUST fail in that build; see
// test_drain_registry_negative_control.

#include "drain_registry.h"

#include <cstdio>

namespace bridge {

const char* const kDrainTicketKey = "_pipeline_ticket";

namespace {

// Local, so this TU stays free of every other bridge header.
std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\u%04x", uc);
            out += esc;
        } else if (c == '"')       out += "\\\"";
        else if (c == '\\')        out += "\\\\";
        else                       out += c;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Payload store
// ---------------------------------------------------------------------------
void DrainRegistry::put(uint64_t ticket, const PendingCall& pc) {
    std::lock_guard<std::mutex> lk(mu_);
    payloads_[ticket] = pc;
}

bool DrainRegistry::take(uint64_t ticket, PendingCall* out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = payloads_.find(ticket);
    if (it == payloads_.end()) return false;
    if (out) *out = it->second;
    payloads_.erase(it);
    return true;
}

bool DrainRegistry::peek(uint64_t ticket, PendingCall* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = payloads_.find(ticket);
    if (it == payloads_.end()) return false;
    if (out) *out = it->second;
    return true;
}

bool DrainRegistry::has_payload(uint64_t ticket) const {
    std::lock_guard<std::mutex> lk(mu_);
    return payloads_.find(ticket) != payloads_.end();
}

size_t DrainRegistry::payload_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return payloads_.size();
}

bool DrainRegistry::drop(uint64_t ticket) {
    std::lock_guard<std::mutex> lk(mu_);
    return payloads_.erase(ticket) != 0;
}

// ---------------------------------------------------------------------------
// Envelope construction — the bridge-minted tag rides on the ENVELOPE
// ---------------------------------------------------------------------------
//
// NOT on the inner action. Tagging the inner action would put a key the client
// never wrote in front of every handler and inside action_args resolution; the
// envelope is bridge-to-bridge and no handler ever sees it.
std::string DrainRegistry::build_envelope(const std::string& session_id,
                                          const std::string& inner_json,
                                          uint64_t ticket) {
    std::string env = "{\"session_id\":\"";
    env += escape_json(session_id);
    env += "\"";
#if !defined(CK3_DRAIN_LEGACY_RID_KEY)
    env += ",\"";
    env += kDrainTicketKey;
    env += "\":";
    env += std::to_string(ticket);
#else
    (void)ticket;   // NEGATIVE CONTROL: the pre-fix envelope carried no tag.
#endif
    env += ",\"actions\":[";
    env += inner_json;
    env += "]}";
    return env;
}

uint64_t DrainRegistry::ticket_from_envelope(const rapidjson::Value& envelope) {
    if (!envelope.IsObject()) return 0;
    if (!envelope.HasMember(kDrainTicketKey)) return 0;
    const rapidjson::Value& v = envelope[kDrainTicketKey];
    if (v.IsUint64()) return v.GetUint64();
    if (v.IsUint())   return static_cast<uint64_t>(v.GetUint());
    if (v.IsInt64() && v.GetInt64() > 0) return static_cast<uint64_t>(v.GetInt64());
    if (v.IsInt()   && v.GetInt()   > 0) return static_cast<uint64_t>(v.GetInt());
    return 0;
}

uint64_t DrainRegistry::ticket_from_envelope(const char* envelope_json) {
    if (!envelope_json || !envelope_json[0]) return 0;
    rapidjson::Document d;
    d.Parse(envelope_json);
    if (d.HasParseError()) return 0;
    return ticket_from_envelope(d);
}

// ---------------------------------------------------------------------------
// Hand-off and settlement
// ---------------------------------------------------------------------------
bool DrainRegistry::hand_off(uint64_t ticket, std::string* envelope_out) {
    PendingCall pc;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = payloads_.find(ticket);
        if (it == payloads_.end()) return false;
        pc = it->second;
        // The payload stays until the drain reports back: it is the record
        // that this ticket is mid-flight on the game thread.
#if defined(CK3_DRAIN_LEGACY_RID_KEY)
        // NEGATIVE CONTROL — the pre-fix line, verbatim. No else. A call whose
        // client sent no request_id is registered NOWHERE and can never settle;
        // two calls sharing one request_id collide and the first is orphaned.
        if (!pc.request_id.empty()) rid_waiting_[pc.request_id] = ticket;
#else
        outstanding_.insert(ticket);
#endif
    }
    if (envelope_out)
        *envelope_out = build_envelope(pc.session_id, pc.inner_json, ticket);
    return true;
}

bool DrainRegistry::outstanding(uint64_t ticket) const {
    std::lock_guard<std::mutex> lk(mu_);
#if defined(CK3_DRAIN_LEGACY_RID_KEY)
    for (const auto& kv : rid_waiting_) if (kv.second == ticket) return true;
    return false;
#else
    return outstanding_.find(ticket) != outstanding_.end();
#endif
}

size_t DrainRegistry::outstanding_count() const {
    std::lock_guard<std::mutex> lk(mu_);
#if defined(CK3_DRAIN_LEGACY_RID_KEY)
    return rid_waiting_.size();
#else
    return outstanding_.size();
#endif
}

bool DrainRegistry::settle(uint64_t ticket) {
    std::lock_guard<std::mutex> lk(mu_);
#if defined(CK3_DRAIN_LEGACY_RID_KEY)
    bool found = false;
    for (auto it = rid_waiting_.begin(); it != rid_waiting_.end(); ++it) {
        if (it->second == ticket) { rid_waiting_.erase(it); found = true; break; }
    }
    if (!found) return false;
#else
    if (outstanding_.erase(ticket) == 0) return false;
#endif
    payloads_.erase(ticket);
    return true;
}

uint64_t DrainRegistry::settle_from_drain(const rapidjson::Value& envelope,
                                          const char* request_id) {
#if defined(CK3_DRAIN_LEGACY_RID_KEY)
    // NEGATIVE CONTROL — the pre-fix settle: correlate by the client's
    // request_id, and give up entirely when there is none.
    (void)envelope;
    if (!request_id || !request_id[0]) return 0;
    uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = rid_waiting_.find(request_id);
        if (it == rid_waiting_.end()) return 0;
        ticket = it->second;
        rid_waiting_.erase(it);
        payloads_.erase(ticket);
    }
    return ticket;
#else
    (void)request_id;   // NEVER used to correlate. That was the bug.
    const uint64_t ticket = ticket_from_envelope(envelope);
    if (ticket == 0) return 0;          // not a conductor envelope
    return settle(ticket) ? ticket : 0; // 0 also covers a double settle
#endif
}

uint64_t DrainRegistry::settle_from_drain(const char* envelope_json,
                                          const char* request_id) {
    if (!envelope_json || !envelope_json[0]) return 0;
    rapidjson::Document d;
    d.Parse(envelope_json);
    if (d.HasParseError()) return 0;
    return settle_from_drain(d, request_id);
}

void DrainRegistry::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    payloads_.clear();
#if defined(CK3_DRAIN_LEGACY_RID_KEY)
    rid_waiting_.clear();
#else
    outstanding_.clear();
#endif
}

}  // namespace bridge
