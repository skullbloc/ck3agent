// board.cpp — see board.h (ck-6kvq.12, R4).
//
// PURE. No engine dependency, no calendar, no globals. The one thing this file
// knows about the outside world is the environment, and only inside
// board_init_from_env().

#include "board.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "action_args.h"
#include "episode_policy.h"
#include "rapidjson/document.h"

namespace bridge {

namespace {

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", uc);
            out += esc;
        } else if (c == '"')  out += "\\\"";
        else if (c == '\\')   out += "\\\\";
        else                  out += c;
    }
    return out;
}

std::string post_id_for(uint64_t seq) {
    char buf[24];
    snprintf(buf, sizeof(buf), "bp_%08llu", static_cast<unsigned long long>(seq));
    return std::string(buf);
}

// Render one post as a JSON object. `episode_id` is emitted only when non-empty
// (the wire shape does not repeat it per post; board.jsonl does, per §6.1).
std::string post_json(const BoardPost& p, const std::string& episode_id) {
    std::string o = "{";
    if (!episode_id.empty()) {
        o += "\"episode_id\":\"" + json_escape(episode_id) + "\",";
    }
    o += "\"seq\":" + std::to_string(p.seq);
    o += ",\"post_id\":\"" + json_escape(p.post_id) + "\"";
    o += ",\"session_id\":\"" + json_escape(p.who.session_id) + "\"";
    o += ",\"character_id\":" + std::to_string(p.who.character_id);
    o += ",\"game_date\":\"" + json_escape(p.who.game_date) + "\"";
    o += ",\"tick\":" + std::to_string(p.who.tick);
    o += ",\"flush_id\":" + std::to_string(p.who.flush_id);
    o += ",\"text\":\"" + json_escape(p.text) + "\"";
    o += "}";
    return o;
}

// What `env_size` did, not merely what it returned (ck-5gdx).
//
// The ARITHMETIC BELOW IS UNCHANGED, deliberately: this bead witnesses the
// resolution, it does not alter it. What changes is that the two silent paths —
// an unparseable value and a non-positive one, both of which keep the default —
// now leave a trace. They were the bead's own finding: an operator action with
// no effect and no record.
struct EnvSizeRead {
    bool        set = false;   // the variable was PRESENT (even if empty)
    std::string raw;           // verbatim, when set
    bool        honoured = false;  // the value reached the config
    size_t      value = 0;     // what the config gets either way
};

EnvSizeRead env_size_read(const char* name, size_t dflt) {
    EnvSizeRead r;
    r.value = dflt;
    const char* v = getenv(name);
    if (!v) return r;                       // absent
    r.set = true;
    r.raw = v;
    if (!*v) return r;                      // exported empty: set, not honoured
    char* end = nullptr;
    long long parsed = strtoll(v, &end, 10);
    if (!end || *end != '\0' || parsed <= 0) return r;   // a typo keeps the default
    r.honoured = true;
    r.value = static_cast<size_t>(parsed);
    return r;
}


// One bound's witness row. THE CLASSIFICATION, stated once:
//
//   not set                                  -> absent
//   set, honoured, byte-equal to declared    -> agreed
//   set, honoured, any other spelling        -> disagreed
//   set, NOT honoured (empty/typo/<=0)       -> refused
//
// The `agreed`/`disagreed` split is on BYTES and not on the resolved number, so
// `02048` is `disagreed` with `in_force == pre_registered`. See board.h.
BoardBoundWitness bound_witness(const char* bound, const char* env_key,
                                size_t pre_registered, const EnvSizeRead& r) {
    BoardBoundWitness b;
    b.bound = bound;
    b.env_key = env_key;
    b.pre_registered = static_cast<uint64_t>(pre_registered);
    b.in_force = static_cast<uint64_t>(r.value);
    b.ambient_set = r.set;
    b.ambient_raw = r.raw;
    b.honoured = r.honoured;
    if (!r.set) {
        b.ambient_state = board_ambient::kAbsent;
    } else if (!r.honoured) {
        b.ambient_state = board_ambient::kRefused;
    } else if (r.raw == std::to_string(pre_registered)) {
        b.ambient_state = board_ambient::kAgreed;
    } else {
        b.ambient_state = board_ambient::kDisagreed;
    }
    return b;
}

// Written once by board_init_from_env() before any client can connect, read
// afterwards. Default-constructed means `observed:false` — never a set of
// plausible-looking defaults, which is the whole ruling this bead turns on.
BoardBoundsWitness& bounds_witness_storage() {
    static BoardBoundsWitness w;
    return w;
}

constexpr size_t kDefaultPageSize = 200;

}  // namespace

// Shared with board_persist.cpp so the carrier and the wire render a post with
// ONE piece of code (declared in board.h).
namespace board_detail {
std::string escape(const std::string& s) { return json_escape(s); }
std::string render_post(const BoardPost& p, const std::string& episode_id) {
    return post_json(p, episode_id);
}
}  // namespace board_detail

bool board_posts_are_dense(const std::vector<BoardPost>& posts) {
    for (size_t i = 0; i < posts.size(); ++i)
        if (posts[i].seq != i) return false;
    return true;
}

const char* board_continuity_name(BoardContinuity c) {
    switch (c) {
        case BoardContinuity::Fresh:            return "fresh";
        case BoardContinuity::AwaitingIdentity: return "awaiting_save_identity";
        case BoardContinuity::Restored:         return "restored";
        case BoardContinuity::Refused:          return "refused";
    }
    return "unknown";
}

std::string BoardRelaunch::json() const {
    std::string o = "{\"at_seq\":" + std::to_string(at_seq);
    o += ",\"epoch\":" + std::to_string(epoch);
    o += ",\"carrier_posts\":" + std::to_string(carrier_posts);
    o += ",\"restored_posts\":" + std::to_string(restored_posts);
    o += ",\"decision\":\"" + json_escape(decision) + "\"";
    o += ",\"why\":\"" + json_escape(why) + "\"";
    o += ",\"episode_id\":\"" + json_escape(episode_id) + "\"";
    o += ",\"save_sha256\":\"" + json_escape(save_sha256) + "\"";
    o += "}";
    return o;
}

// ============================================================================
// Configuration
// ============================================================================
void EpisodeBoard::configure(const BoardConfig& c) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = c;
    enabled_.store(c.enabled, std::memory_order_relaxed);
}

BoardConfig EpisodeBoard::config() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
}

// ============================================================================
// Per-episode lifecycle
// ============================================================================
void EpisodeBoard::begin_episode(const std::string& episode_id) {
    std::lock_guard<std::mutex> lk(mu_);
    episode_id_ = episode_id;
    sealed_ = false;
    posts_.clear();
    per_flush_.clear();
    next_seq_ = 0;              // §3.2: a new episode starts at seq 0
    refusals_ = 0;
    highest_flush_seen_ = 0;
    // ck-81su: an explicit begin is the one thing allowed to discard a board, so
    // it also clears the continuity latch. The RELAUNCH path does not come
    // through here — see board_carrier_init_from_env().
    continuity_ = BoardContinuity::Fresh;
    carrier_posts_ = 0;
    carrier_next_seq_ = 0;
    relaunches_.clear();
}

void EpisodeBoard::seal() {
    std::lock_guard<std::mutex> lk(mu_);
    sealed_ = true;
}

bool EpisodeBoard::sealed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sealed_;
}

std::string EpisodeBoard::episode_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return episode_id_;
}

// ============================================================================
// The one-field parse (the forgery rule)
// ============================================================================
bool board_parse_post_text(const char* params_json, std::string* text_out) {
    if (text_out) text_out->clear();
    if (!params_json || !*params_json) return false;

    rapidjson::Document d;
    if (d.Parse(params_json).HasParseError() || !d.IsObject()) return false;

    // action_args resolves flat-vs-framed PER KEY (ck-6kvq.15): a raw-socket
    // caller sends {"action":"board_post","text":"..."} and every real client
    // sends {"action":"board_post","params":{"text":"..."}}. A hand-rolled
    // d["text"] would see only the first, and would answer an empty post to
    // every MCP call — which is precisely how `check` shipped broken.
    if (!action_has(d, "text")) return false;
    const rapidjson::Value& v = action_args(d, "text");
    if (!v.HasMember("text")) return false;
    const rapidjson::Value& t = v["text"];
    if (!t.IsString()) return false;
    if (text_out) text_out->assign(t.GetString(), t.GetStringLength());
    return true;

    // NOTE, deliberately: there is no read of "session_id", "character_id",
    // "game_date", "tick" or "flush_id" anywhere in this function, and there is
    // no other parse of a board frame in this module. That absence IS the
    // forgery rule — see board.h.
}

// ============================================================================
// post
// ============================================================================
BoardPostResult EpisodeBoard::refuse_locked(const char* reason, const char* quota,
                                            int64_t limit, int64_t observed) {
    ++refusals_;
    BoardPostResult r;
    r.ok = false;
    r.reason = reason ? reason : "";
    r.quota = quota ? quota : "";
    r.limit = limit;
    r.observed = observed;
    return r;
}

BoardPostResult EpisodeBoard::post(const char* params_json,
                                   const BoardAttribution& minted) {
    std::string text;
    if (!board_parse_post_text(params_json, &text)) {
        std::lock_guard<std::mutex> lk(mu_);
        return refuse_locked(board_reason::kEmptyText, nullptr, 0, 0);
    }
    return post_text(text, minted);
}

BoardPostResult EpisodeBoard::post_text(const std::string& text,
                                        const BoardAttribution& minted) {
    std::lock_guard<std::mutex> lk(mu_);

    if (!cfg_.enabled)
        return refuse_locked(board_reason::kDisabled, nullptr, 0, 0);

    // ck-81su: THE CONTINUITY GATE, and it is checked BEFORE `sealed_` so the
    // refusal names the actual situation rather than a generic seal.
    //
    // This process found a board carrier for this episode, which means a
    // previous process already minted seqs for this board. Accepting a post now
    // would mint seq 0 a second time and start a parallel chain — the exact
    // damage this bead exists to stop, and the damage is WORSE than the refusal
    // because it is invisible in a bundle that still validates. So: refuse,
    // typed, until a declaration resolves the carrier one way or the other.
    if (continuity_ == BoardContinuity::AwaitingIdentity) {
        return refuse_locked(board_reason::kAwaitingIdentity, nullptr,
                             static_cast<int64_t>(carrier_next_seq_),
                             static_cast<int64_t>(carrier_posts_));
    }
    if (continuity_ == BoardContinuity::Refused) {
        return refuse_locked(board_reason::kIdentityRefused, nullptr,
                             static_cast<int64_t>(carrier_next_seq_),
                             static_cast<int64_t>(carrier_posts_));
    }

    if (sealed_)
        return refuse_locked(board_reason::kSealed, nullptr, 0, 0);

    // The minted session id is the ONLY identity that exists here. A post with
    // no session behind it has no author, and an unauthored post in an
    // append-only record is worse than no post.
    if (minted.session_id.empty())
        return refuse_locked(board_reason::kNoSession, nullptr, 0, 0);

    if (text.empty())
        return refuse_locked(board_reason::kEmptyText, nullptr, 0, 0);

    if (text.size() > cfg_.max_post_bytes) {
        return refuse_locked(board_reason::kQuotaExceeded, board_quota::kPostBytes,
                             static_cast<int64_t>(cfg_.max_post_bytes),
                             static_cast<int64_t>(text.size()));
    }

    if (posts_.size() >= cfg_.max_posts_per_episode) {
        return refuse_locked(board_reason::kQuotaExceeded, board_quota::kPostsPerEpisode,
                             static_cast<int64_t>(cfg_.max_posts_per_episode),
                             static_cast<int64_t>(posts_.size()) + 1);
    }

    // FLUSH SCOPE. See BoardConfig::require_flush_scope: an experiment episode
    // refuses an unanchored post rather than recording a row nothing can join.
    if (minted.flush_id == 0 && cfg_.require_flush_scope)
        return refuse_locked(board_reason::kOutsideFlush, nullptr, 0, 0);

    // Per (session, flush). flush_id 0 means "not inside a flush" — legacy and
    // null pipeline modes — where this bound has no meaning and only the
    // per-episode ceiling applies. An experiment episode runs in pipeline mode
    // by construction, so every post it records is flush-scoped.
    if (minted.flush_id != 0) {
        if (minted.flush_id > highest_flush_seen_) {
            // The flush advanced: drop every older counter rather than keep one
            // entry per (session, flush) for a 20-game-year run.
            for (auto it = per_flush_.begin(); it != per_flush_.end(); ) {
                if (it->first.second < minted.flush_id) it = per_flush_.erase(it);
                else ++it;
            }
            highest_flush_seen_ = minted.flush_id;
        }
        auto key = std::make_pair(minted.session_id, minted.flush_id);
        size_t already = per_flush_.count(key) ? per_flush_[key] : 0;
        if (already >= cfg_.max_posts_per_session_per_flush) {
            return refuse_locked(board_reason::kQuotaExceeded,
                                 board_quota::kPostsPerFlush,
                                 static_cast<int64_t>(cfg_.max_posts_per_session_per_flush),
                                 static_cast<int64_t>(already) + 1);
        }
        per_flush_[key] = already + 1;
    }

    BoardPost p;
    p.seq     = next_seq_++;
    p.post_id = post_id_for(p.seq);
    p.who     = minted;          // WHOLESALE. Nothing from the client frame.
    p.text    = text;
    posts_.push_back(p);

    // ck-81su: the carrier is written INSIDE the board mutex, in the same
    // critical section that minted the seq, so what is on disk can never be a
    // reordering of what is in memory. board_carrier_append_post() is a LEAF —
    // it takes only its own mutex and never calls back in here.
    board_carrier_append_post(p);

    BoardPostResult r;
    r.ok = true;
    r.post = p;
    return r;
}

// ============================================================================
// read
// ============================================================================
BoardReadResult EpisodeBoard::read(uint64_t since_seq, size_t limit) const {
    std::lock_guard<std::mutex> lk(mu_);
    BoardReadResult out;
    out.total = posts_.size();
    if (limit == 0) limit = kDefaultPageSize;

    // posts_ is append-only and seq is assigned monotonically from 0, so
    // posts_[i].seq == i and the cursor is an index. Asserted in the unit test
    // rather than assumed here.
    size_t start = (since_seq >= posts_.size()) ? posts_.size()
                                                : static_cast<size_t>(since_seq);
    for (size_t i = start; i < posts_.size() && out.posts.size() < limit; ++i)
        out.posts.push_back(posts_[i]);

    out.next_seq = out.posts.empty() ? (since_seq > next_seq_ ? next_seq_ : since_seq)
                                     : out.posts.back().seq + 1;
    out.truncated = out.next_seq < next_seq_;
    return out;
}

std::string EpisodeBoard::read_json(uint64_t since_seq, size_t limit) const {
    BoardReadResult res = read(since_seq, limit);
    std::string o = "{\"posts\":[";
    for (size_t i = 0; i < res.posts.size(); ++i) {
        if (i) o += ",";
        o += post_json(res.posts[i], std::string());
    }
    o += "],\"next_seq\":" + std::to_string(res.next_seq);
    o += ",\"total\":" + std::to_string(res.total);
    o += ",\"truncated\":";
    o += res.truncated ? "true" : "false";
    // ck-81su: ALWAYS emitted, even when nothing has ever relaunched. A reader
    // that has to guess whether the key exists cannot tell "this board is
    // continuous" from "this bridge does not know the question".
    {
        std::lock_guard<std::mutex> lk(mu_);
        o += ",\"continuity\":" + continuity_json_locked();
    }
    o += "}";
    return o;
}

std::string EpisodeBoard::export_jsonl() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string o;
    for (const BoardPost& p : posts_) {
        o += post_json(p, episode_id_);
        o += "\n";
    }
    return o;
}

std::string EpisodeBoard::stats_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, size_t> by_session;
    for (const BoardPost& p : posts_) by_session[p.who.session_id]++;

    std::string o = "{\"enabled\":";
    o += cfg_.enabled ? "true" : "false";
    o += ",\"episode_id\":\"" + json_escape(episode_id_) + "\"";
    o += ",\"sealed\":";
    o += sealed_ ? "true" : "false";
    o += ",\"posts\":" + std::to_string(posts_.size());
    o += ",\"next_seq\":" + std::to_string(next_seq_);
    o += ",\"refusals\":" + std::to_string(refusals_);
    o += ",\"max_post_bytes\":" + std::to_string(cfg_.max_post_bytes);
    o += ",\"max_posts_per_session_per_flush\":" +
         std::to_string(cfg_.max_posts_per_session_per_flush);
    o += ",\"max_posts_per_episode\":" + std::to_string(cfg_.max_posts_per_episode);
    o += ",\"require_flush_scope\":";
    o += cfg_.require_flush_scope ? "true" : "false";
    // ck-5gdx: WHAT MANIPULATION THIS EPISODE IS RUNNING. The three numbers
    // above are the effect; this is the operator action behind each of them.
    o += ",\"bounds_witness\":" + bounds_witness_storage().json();
    o += ",\"continuity\":" + continuity_json_locked();   // ck-81su
    // ck-hzp7: WHAT THE CARRIER FAILED TO WRITE. A POSITIVE RECORD OF
    // INCOMPLETENESS, published HERE — over the socket — because the failure it
    // reports is a disk that will not take writes, and an error channel that
    // shares a cause with the error cannot report it.
    //
    // ITS ABSENCE IS NOT EVIDENCE OF COMPLETENESS. A bundle sealed before this
    // bead carries no `carrier` block at all, and on a device so full that the
    // BUNDLE cannot be written either, nothing is recorded anywhere — which is
    // honest-no-bundle, not degraded-with-count. `degraded:false` means the
    // carrier looked and dropped nothing; a MISSING block means nobody looked.
    {
        const CarrierHealth ch = board_carrier_health();
        o += ",\"carrier\":{\"degraded\":";
        o += ch.degraded ? "true" : "false";
        o += ",\"records_dropped\":" + std::to_string(ch.records_dropped);
        if (!ch.reason.empty())
            o += ",\"reason\":\"" + json_escape(ch.reason) + "\"";
        o += "}";
    }
    o += ",\"by_session\":{";
    bool first = true;
    for (const auto& kv : by_session) {
        if (!first) o += ",";
        first = false;
        o += "\"" + json_escape(kv.first) + "\":" + std::to_string(kv.second);
    }
    o += "}}";
    return o;
}

// ============================================================================
// Continuity across a relaunch (ck-81su)
// ============================================================================
// Caller must hold mu_.
std::string EpisodeBoard::continuity_json_locked() const {
    std::string o = "{\"state\":\"";
    o += board_continuity_name(continuity_);
    o += "\",\"carrier_posts\":" + std::to_string(carrier_posts_);
    o += ",\"carrier_next_seq\":" + std::to_string(carrier_next_seq_);
    o += ",\"relaunches\":[";
    for (size_t i = 0; i < relaunches_.size(); ++i) {
        if (i) o += ",";
        o += relaunches_[i].json();
    }
    o += "]}";
    return o;
}

BoardContinuity EpisodeBoard::continuity() const {
    std::lock_guard<std::mutex> lk(mu_);
    return continuity_;
}

std::vector<BoardRelaunch> EpisodeBoard::relaunches() const {
    std::lock_guard<std::mutex> lk(mu_);
    return relaunches_;
}

void EpisodeBoard::await_carrier(size_t carrier_posts, uint64_t carrier_next_seq) {
    std::lock_guard<std::mutex> lk(mu_);
    continuity_       = BoardContinuity::AwaitingIdentity;
    carrier_posts_    = carrier_posts;
    carrier_next_seq_ = carrier_next_seq;
}

size_t EpisodeBoard::adopt_carrier(std::vector<BoardPost> posts, uint64_t next_seq,
                                   const BoardRelaunch& rec) {
    std::lock_guard<std::mutex> lk(mu_);

    // The storage invariant, enforced where the storage lives. A caller that
    // somehow reached here with a holed chain adopts NOTHING rather than
    // installing rows read() would mis-index. board_carrier_accept() checks the
    // same predicate first so it can record a typed refusal; this is the net
    // under it, and both call the one function.
    if (!board_posts_are_dense(posts)) return 0;

    // THE HIGH-WATER MARK IS THE POINT. Restoring the rows and letting seq
    // restart at 0 would produce a board.jsonl with two posts numbered 0, which
    // is a worse record than the truncation this replaces.
    posts_    = std::move(posts);
    next_seq_ = next_seq > posts_.size() ? next_seq : posts_.size();

    // The per-flush counters are NOT restored, and that is deliberate: a flush
    // does not span a process relaunch, so there is no flush whose quota this
    // process could still be inside. Restoring them would refuse a legitimate
    // first post after a resume.
    per_flush_.clear();
    highest_flush_seen_ = 0;

    sealed_        = false;
    continuity_    = BoardContinuity::Restored;
    relaunches_.push_back(rec);
    return posts_.size();
}

void EpisodeBoard::refuse_carrier(const BoardRelaunch& rec) {
    std::lock_guard<std::mutex> lk(mu_);
    continuity_ = BoardContinuity::Refused;
    sealed_     = true;   // reads still work; the harness can still drain
    relaunches_.push_back(rec);
}

size_t EpisodeBoard::total_posts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return posts_.size();
}

size_t EpisodeBoard::total_refusals() const {
    std::lock_guard<std::mutex> lk(mu_);
    return refusals_;
}

uint64_t EpisodeBoard::next_seq() const {
    std::lock_guard<std::mutex> lk(mu_);
    return next_seq_;
}

// ============================================================================
// Process-wide instance + env resolution
// ============================================================================
EpisodeBoard& board() {
    static EpisodeBoard b;
    return b;
}

const BoardBoundsWitness& board_bounds_witness() { return bounds_witness_storage(); }

std::string BoardBoundsWitness::json() const {
    std::string o = "{\"observed\":";
    o += observed ? "true" : "false";
    o += ",\"bounds\":[";
    bool first = true;
    for (const BoardBoundWitness& b : bounds) {
        if (!first) o += ",";
        first = false;
        o += "{\"bound\":\"" + json_escape(b.bound) + "\"";
        o += ",\"env_key\":\"" + json_escape(b.env_key) + "\"";
        o += ",\"pre_registered\":" + std::to_string(b.pre_registered);
        o += ",\"in_force\":" + std::to_string(b.in_force);
        o += ",\"ambient_state\":\"" + json_escape(b.ambient_state) + "\"";
        // NULL when the variable was absent, and never "" — an exported empty
        // string is a real and different operator action (ck-zixt's rule, and
        // here it is one of the `refused` cases).
        o += ",\"ambient_raw\":";
        if (b.ambient_set) o += "\"" + json_escape(b.ambient_raw) + "\"";
        else               o += "null";
        o += ",\"honoured\":";
        o += b.honoured ? "true" : "false";
        o += "}";
    }
    o += "]}";
    return o;
}

bool is_board_action(const char* action_type) {
    if (!action_type || !action_type[0]) return false;
    return strcmp(action_type, "board_post") == 0 ||
           strcmp(action_type, "board_read") == 0;
}

int board_verb_registration(const char* action_type) {
    if (!action_type || !action_type[0]) return -1;
    if (is_board_action(action_type)) return board().enabled() ? 1 : 0;
    if (strcmp(action_type, "board_admin") == 0) {
        const char* tok = getenv("CK3_BOARD_ADMIN_TOKEN");
        return (board().enabled() && tok && *tok) ? 1 : 0;
    }
    return -1;
}

void board_init_from_env() {
    BoardConfig c;

    // THE ARM GATE (G5 / A2). The arm is episode config, resolved by
    // episode_policy from CK3_EPISODE_ARM; the board exists in arm `b` and
    // nowhere else. CK3_BOARD_ENABLED is an explicit override so a host test
    // can arm the board without pretending to be an episode — it can only be
    // consulted when it is actually set.
    const EpisodePolicy p = episode_policy().get();
    c.enabled = (p.arm == "b");
    if (const char* ov = getenv("CK3_BOARD_ENABLED")) {
        if (*ov) c.enabled = !(strcmp(ov, "0") == 0 || strcmp(ov, "false") == 0);
    }

    // ck-5gdx: resolve each bound AND record what the environment tried to do
    // to it. The three reads below are the same arithmetic as before; the
    // witness is the part that was missing.
    const EnvSizeRead rb = env_size_read("CK3_BOARD_MAX_POST_BYTES", c.max_post_bytes);
    const EnvSizeRead rf = env_size_read("CK3_BOARD_MAX_POSTS_PER_FLUSH",
                                         c.max_posts_per_session_per_flush);
    const EnvSizeRead re = env_size_read("CK3_BOARD_MAX_POSTS_PER_EPISODE",
                                         c.max_posts_per_episode);
    // The PRE-REGISTERED values are the defaults a freshly constructed
    // BoardConfig carries — read from `BoardConfig{}` rather than retyped here,
    // so the witness cannot claim a declared value board.h does not hold.
    const BoardConfig declared;
    BoardBoundsWitness w;
    w.observed = true;
    w.bounds.push_back(bound_witness("max_post_bytes", "CK3_BOARD_MAX_POST_BYTES",
                                     declared.max_post_bytes, rb));
    w.bounds.push_back(bound_witness("max_posts_per_session_per_flush",
                                     "CK3_BOARD_MAX_POSTS_PER_FLUSH",
                                     declared.max_posts_per_session_per_flush, rf));
    w.bounds.push_back(bound_witness("max_posts_per_episode",
                                     "CK3_BOARD_MAX_POSTS_PER_EPISODE",
                                     declared.max_posts_per_episode, re));
    bounds_witness_storage() = w;

    c.max_post_bytes                  = rb.value;
    c.max_posts_per_session_per_flush = rf.value;
    c.max_posts_per_episode           = re.value;

    // An EXPERIMENT episode runs the pipeline, so every post it records is
    // flush-anchored; free play has no flushes and must not be refused for it.
    c.require_flush_scope = (p.mode == EpisodeMode::Experiment);

    board().configure(c);
    board().begin_episode(p.episode_id);

    // ck-81su: and THEN ask whether this process is a relaunch. begin_episode()
    // above has just reset seq to 0 — which is right for a cold start and was
    // the whole bug for a resume — so the carrier is consulted immediately
    // after it, before any client can connect and post. With no board, no
    // episode directory or no carrier this is a no-op and the board is Fresh.
    board_carrier_init_from_env();
}

}  // namespace bridge
