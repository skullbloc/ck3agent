// test/test_board.cpp — ck-6kvq.12 (R4) unit tests for the shared board.
//
// PURE: no engine, no CK3, no calendar. Everything the board promises is a
// property of bridge/src/board.cpp plus the episode policy that carries the
// ARM, and both are host code.
//
// This file ASSERTS (first violation aborts) — it is the regression gate.
// bridge/test/acceptance_board.cpp REPORTS every check and is the re-runnable
// evidence; scripts/acceptance_board.py drives that one plus the arm-gate lane
// and the negative control.

#include "board.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "episode_policy.h"

// ck-81su: board_persist.cpp logs through the bridge log. Stubbed here — the
// same stub acceptance_board.cpp provides — so the carrier compiles standalone.
namespace bridge { FILE* g_logfile = nullptr; }

using namespace bridge;

static int g_checks = 0;
#define CHECK(cond, what)                                                      \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s  (%s:%d)\n", what, __FILE__, __LINE__);  \
            abort();                                                           \
        }                                                                      \
    } while (0)

static BoardConfig armed_config() {
    BoardConfig c;
    c.enabled = true;
    c.max_post_bytes = 64;
    c.max_posts_per_session_per_flush = 2;
    c.max_posts_per_episode = 10;
    return c;
}

static BoardAttribution who(const char* sid, uint32_t cid, const char* date,
                            uint64_t tick, uint64_t flush) {
    BoardAttribution a;
    a.session_id = sid;
    a.character_id = cid;
    a.game_date = date;
    a.tick = tick;
    a.flush_id = flush;
    return a;
}

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Append-only, monotonic seq, per-episode reset
// ---------------------------------------------------------------------------
static void test_append_only_and_seq() {
    EpisodeBoard b;
    b.configure(armed_config());
    b.begin_episode("ep-1");

    CHECK(b.next_seq() == 0, "a fresh episode starts at seq 0");
    CHECK(b.total_posts() == 0, "a fresh episode has no posts");

    BoardPostResult r0 = b.post_text("muster at York", who("s1", 101, "1066.10.14", 7, 3));
    CHECK(r0.ok, "first post accepted");
    CHECK(r0.post.seq == 0, "FIRST POST IS SEQ 0");
    CHECK(r0.post.post_id == "bp_00000000", "post_id derives from seq");

    BoardPostResult r1 = b.post_text("holding Mercia", who("s2", 102, "1066.10.14", 7, 3));
    CHECK(r1.ok && r1.post.seq == 1, "seq is monotonic across sessions");

    BoardPostResult r2 = b.post_text("third", who("s3", 103, "1066.10.15", 8, 4));
    CHECK(r2.ok && r2.post.seq == 2, "seq keeps climbing across flushes");

    // ORDER is insertion order, and read() returns it unchanged.
    BoardReadResult all = b.read(0, 0);
    CHECK(all.posts.size() == 3, "read(0) returns every post");
    CHECK(all.posts[0].seq == 0 && all.posts[1].seq == 1 && all.posts[2].seq == 2,
          "posts come back in seq order");
    CHECK(all.next_seq == 3, "next_seq is last+1");
    CHECK(!all.truncated, "an unlimited read is not truncated");

    // The index==seq invariant read() relies on.
    for (size_t i = 0; i < all.posts.size(); ++i)
        CHECK(all.posts[i].seq == i, "posts_[i].seq == i");

    // A cursor read returns only what is new.
    BoardReadResult tail = b.read(2, 0);
    CHECK(tail.posts.size() == 1 && tail.posts[0].seq == 2, "since_seq is a cursor");
    BoardReadResult none = b.read(3, 0);
    CHECK(none.posts.empty() && none.next_seq == 3, "a caught-up reader gets nothing");

    // Paging.
    BoardReadResult page = b.read(0, 2);
    CHECK(page.posts.size() == 2 && page.next_seq == 2 && page.truncated,
          "a limited read reports truncation and a resume cursor");

    // PER-EPISODE: a new episode starts at seq 0 with an empty board.
    b.begin_episode("ep-2");
    CHECK(b.next_seq() == 0 && b.total_posts() == 0,
          "a NEW EPISODE starts at seq 0 with no posts");
    BoardPostResult again = b.post_text("new episode", who("s1", 101, "1067.01.01", 1, 1));
    CHECK(again.ok && again.post.seq == 0, "the first post of a new episode is seq 0");
}

// ---------------------------------------------------------------------------
// Attribution is MINTED; a client-supplied identity is IGNORED
// ---------------------------------------------------------------------------
static void test_attribution_cannot_be_forged() {
    EpisodeBoard b;
    b.configure(armed_config());
    b.begin_episode("ep-forge");

    // A control session posting as itself.
    const BoardAttribution minted = who("sess_real", 30853, "1066.10.14", 99, 5);

    // ...and a frame that tries to be somebody else entirely.
    const char* forged =
        "{\"action\":\"board_post\",\"params\":{\"text\":\"I am the king\","
        "\"session_id\":\"sess_victim\",\"character_id\":31808,"
        "\"game_date\":\"1200.01.01\",\"tick\":1,\"flush_id\":999}}";

    BoardPostResult r = b.post(forged, minted);
    CHECK(r.ok, "a forged frame is accepted — the forgery is ignored, not refused");
    CHECK(r.post.who.session_id == "sess_real", "session_id is the MINTED one");
    CHECK(r.post.who.character_id == 30853, "character_id is the MINTED one");
    CHECK(r.post.who.game_date == "1066.10.14", "game_date comes from the clock owner");
    CHECK(r.post.who.tick == 99, "tick comes from the clock owner");
    CHECK(r.post.who.flush_id == 5, "flush_id comes from the conductor");
    CHECK(r.post.text == "I am the king", "the TEXT is the only thing taken from the frame");

    // And the parse itself takes exactly one field.
    std::string text;
    CHECK(board_parse_post_text(forged, &text), "the one-field parse succeeds");
    CHECK(text == "I am the king", "the one-field parse returns only the text");

    // FLAT framing (a raw-socket caller) resolves too — the ck-6kvq.15 bug class.
    std::string flat_text;
    CHECK(board_parse_post_text("{\"action\":\"board_post\",\"text\":\"flat\"}", &flat_text),
          "a FLAT frame parses");
    CHECK(flat_text == "flat", "flat framing yields the same text");

    // A frame with no text at all is refused, not silently posted empty.
    std::string nothing;
    CHECK(!board_parse_post_text("{\"action\":\"board_post\",\"params\":{}}", &nothing),
          "a frame with no text does not parse");
    BoardPostResult empty = b.post("{\"params\":{}}", minted);
    CHECK(!empty.ok && empty.reason == board_reason::kEmptyText,
          "a textless post is refused board_empty_text");

    // A non-string text is refused rather than coerced.
    std::string coerced;
    CHECK(!board_parse_post_text("{\"text\":42}", &coerced),
          "a non-string text does not parse");
}

// ---------------------------------------------------------------------------
// Quotas REFUSE, typed; nothing is truncated and nothing is evicted
// ---------------------------------------------------------------------------
static void test_quotas_are_typed_refusals() {
    EpisodeBoard b;
    BoardConfig c = armed_config();
    c.max_post_bytes = 8;
    c.max_posts_per_session_per_flush = 2;
    c.max_posts_per_episode = 4;
    b.configure(c);
    b.begin_episode("ep-quota");

    // max_post_bytes
    BoardPostResult big = b.post_text("123456789", who("s1", 1, "1066.10.14", 1, 1));
    CHECK(!big.ok, "an oversized post is refused");
    CHECK(big.reason == board_reason::kQuotaExceeded, "refusal is board_quota_exceeded");
    CHECK(big.quota == board_quota::kPostBytes, "the refusal names max_post_bytes");
    CHECK(big.limit == 8 && big.observed == 9, "the refusal carries limit and observed");
    CHECK(b.total_posts() == 0, "NOTHING WAS TRUNCATED — the post is simply absent");

    // max_posts_per_session_per_flush, keyed on (session, flush)
    CHECK(b.post_text("a", who("s1", 1, "1066.10.14", 1, 1)).ok, "flush 1 post 1");
    CHECK(b.post_text("b", who("s1", 1, "1066.10.14", 1, 1)).ok, "flush 1 post 2");
    BoardPostResult third = b.post_text("c", who("s1", 1, "1066.10.14", 1, 1));
    CHECK(!third.ok && third.quota == board_quota::kPostsPerFlush,
          "a third post in the same flush is refused typed");
    CHECK(b.post_text("d", who("s2", 2, "1066.10.14", 1, 1)).ok,
          "the quota is PER SESSION — another session in the same flush is fine");
    CHECK(b.post_text("e", who("s1", 1, "1066.10.15", 2, 2)).ok,
          "the quota resets with the flush");

    // max_posts_per_episode
    CHECK(b.total_posts() == 4, "four posts accepted");
    BoardPostResult over = b.post_text("f", who("s3", 3, "1066.10.16", 3, 3));
    CHECK(!over.ok && over.quota == board_quota::kPostsPerEpisode,
          "the episode ceiling refuses typed");
    CHECK(b.total_posts() == 4, "NOTHING WAS EVICTED — append-only survives the ceiling");
    CHECK(b.total_refusals() == 3, "every refusal is counted");
}

// ---------------------------------------------------------------------------
// Flush scope: the surviving form of "a post outside a window is refused"
// ---------------------------------------------------------------------------
static void test_flush_scope() {
    EpisodeBoard b;
    BoardConfig c = armed_config();
    c.require_flush_scope = false;
    b.configure(c);
    b.begin_episode("ep-freeplay");
    CHECK(b.post_text("legacy", who("s1", 1, "1066.10.14", 1, 0)).ok,
          "free play accepts an unanchored post (there are no flushes)");

    c.require_flush_scope = true;
    b.configure(c);
    b.begin_episode("ep-experiment");
    BoardPostResult un = b.post_text("unanchored", who("s1", 1, "1066.10.14", 1, 0));
    CHECK(!un.ok && un.reason == board_reason::kOutsideFlush,
          "an EXPERIMENT episode refuses a post with no flush behind it");
    CHECK(b.post_text("anchored", who("s1", 1, "1066.10.14", 1, 7)).ok,
          "and accepts one that carries a flush");
}

// ---------------------------------------------------------------------------
// Disabled and sealed
// ---------------------------------------------------------------------------
static void test_disabled_and_sealed() {
    EpisodeBoard b;
    BoardConfig c = armed_config();
    c.enabled = false;
    b.configure(c);
    b.begin_episode("ep-armA");
    CHECK(!b.enabled(), "a disabled board reports disabled");
    BoardPostResult r = b.post_text("hello", who("s1", 1, "1066.10.14", 1, 1));
    CHECK(!r.ok && r.reason == board_reason::kDisabled,
          "a disabled board refuses board_disabled (defence in depth — in arm A "
          "the verb is not even registered)");

    c.enabled = true;
    b.configure(c);
    b.begin_episode("ep-seal");
    CHECK(b.post_text("before", who("s1", 1, "1066.10.14", 1, 1)).ok, "post before seal");
    b.seal();
    CHECK(b.sealed(), "seal() latches");
    BoardPostResult after = b.post_text("after", who("s1", 1, "1066.10.14", 1, 1));
    CHECK(!after.ok && after.reason == board_reason::kSealed,
          "a sealed board refuses further posts");
    CHECK(b.read(0, 0).posts.size() == 1, "but a sealed board still READS");

    // An author-less post has no attribution to record.
    b.begin_episode("ep-anon");
    BoardPostResult anon = b.post_text("who am i", who("", 0, "1066.10.14", 1, 1));
    CHECK(!anon.ok && anon.reason == board_reason::kNoSession,
          "a post with no minted session is refused board_no_session");
}

// ---------------------------------------------------------------------------
// Wire and bundle shapes
// ---------------------------------------------------------------------------
static void test_shapes() {
    EpisodeBoard b;
    b.configure(armed_config());
    b.begin_episode("ep-20260912T000000Z-b-deadbeef");
    b.post_text("hold the \"line\"\n", who("sess_a", 30853, "1066.10.14", 12, 4));
    b.post_text("riding north", who("sess_b", 32624, "1066.10.15", 13, 5));

    const std::string wire = b.read_json(0, 0);
    for (const char* k : {"\"posts\":", "\"next_seq\":2", "\"total\":2",
                          "\"truncated\":false", "\"seq\":0", "\"post_id\":\"bp_00000000\"",
                          "\"session_id\":\"sess_a\"", "\"character_id\":30853",
                          "\"game_date\":\"1066.10.14\"", "\"tick\":12",
                          "\"flush_id\":4", "\"text\":\""}) {
        CHECK(contains(wire, k), k);
    }
    // The wire shape does NOT repeat episode_id per post; board.jsonl does.
    CHECK(!contains(wire, "episode_id"), "the wire page carries no per-post episode_id");
    // Control characters and quotes are escaped, not emitted raw.
    CHECK(contains(wire, "\\\"line\\\""), "quotes inside the text are escaped");
    CHECK(contains(wire, "\\u000a"), "a newline inside the text is escaped");

    const std::string jsonl = b.export_jsonl();
    size_t lines = 0;
    for (char ch : jsonl) if (ch == '\n') ++lines;
    CHECK(lines == 2, "board.jsonl is one line per post");
    // §6.1 stamping law: episode_id on EVERY record, and the join keys for
    // actions.jsonl on {episode_id, flush_id, session_id}.
    CHECK(contains(jsonl, "\"episode_id\":\"ep-20260912T000000Z-b-deadbeef\""),
          "every board.jsonl record is stamped with episode_id");
    CHECK(contains(jsonl, "\"flush_id\":4") && contains(jsonl, "\"session_id\":\"sess_a\""),
          "board.jsonl carries the actions.jsonl join keys");

    const std::string stats = b.stats_json();
    for (const char* k : {"\"enabled\":true", "\"posts\":2", "\"next_seq\":2",
                          "\"refusals\":0", "\"by_session\":{",
                          "\"sess_a\":1", "\"sess_b\":1"}) {
        CHECK(contains(stats, k), k);
    }
}

// ---------------------------------------------------------------------------
// The ARM GATE, read off the episode policy
// ---------------------------------------------------------------------------
static void test_arm_gate_from_env() {
    struct Env {
        static void set(const char* k, const char* v) {
            if (v) setenv(k, v, 1); else unsetenv(k);
        }
    };
    Env::set("CK3_BOARD_ENABLED", nullptr);
    Env::set("CK3_EPISODE_MODE", "experiment");
    Env::set("CK3_EPISODE_ID", "ep-arm");

    struct Case { const char* arm; bool enabled; const char* what; };
    const Case cases[] = {
        {nullptr, false, "no arm declared (free play) => NO BOARD"},
        {"e0",    false, "arm E0 => NO BOARD"},
        {"a",     false, "arm A => NO BOARD (G5 option A2: ABSENT)"},
        {"b",     true,  "arm B => the board exists"},
        {"B",     true,  "the arm is case-insensitive"},
        {"c",     false, "arm C => NO BOARD"},
        {"bee",   false, "an unrecognised arm is NOT b => NO BOARD"},
    };
    for (const Case& c : cases) {
        Env::set("CK3_EPISODE_ARM", c.arm);
        episode_policy().init_from_env();
        board_init_from_env();
        CHECK(board().enabled() == c.enabled, c.what);
        CHECK(board().episode_id() == "ep-arm", "the board opens on the episode id");
    }

    // require_flush_scope follows the EPISODE MODE, not the arm.
    Env::set("CK3_EPISODE_ARM", "b");
    Env::set("CK3_EPISODE_MODE", "free_play");
    episode_policy().init_from_env();
    board_init_from_env();
    CHECK(!board().config().require_flush_scope,
          "free play does not require a flush behind a post");
    Env::set("CK3_EPISODE_MODE", "experiment");
    episode_policy().init_from_env();
    board_init_from_env();
    CHECK(board().config().require_flush_scope,
          "an experiment episode does");

    // The explicit override, for host tests that are not episodes.
    Env::set("CK3_EPISODE_ARM", "a");
    Env::set("CK3_BOARD_ENABLED", "1");
    episode_policy().init_from_env();
    board_init_from_env();
    CHECK(board().enabled(), "CK3_BOARD_ENABLED=1 arms the board for a host test");
    Env::set("CK3_BOARD_ENABLED", "0");
    Env::set("CK3_EPISODE_ARM", "b");
    episode_policy().init_from_env();
    board_init_from_env();
    CHECK(!board().enabled(), "CK3_BOARD_ENABLED=0 disarms it even in arm b");

    // The bounds come from the environment too.
    Env::set("CK3_BOARD_ENABLED", "1");
    Env::set("CK3_BOARD_MAX_POST_BYTES", "17");
    Env::set("CK3_BOARD_MAX_POSTS_PER_FLUSH", "4");
    Env::set("CK3_BOARD_MAX_POSTS_PER_EPISODE", "99");
    board_init_from_env();
    CHECK(board().config().max_post_bytes == 17, "max_post_bytes from env");
    CHECK(board().config().max_posts_per_session_per_flush == 4, "per-flush quota from env");
    CHECK(board().config().max_posts_per_episode == 99, "per-episode quota from env");
    Env::set("CK3_BOARD_MAX_POST_BYTES", "banana");
    board_init_from_env();
    CHECK(board().config().max_post_bytes == 2048,
          "an unparseable bound degrades to the documented default, never to 0");

    // is_board_action is the arm gate's name test and knows exactly two verbs.
    CHECK(is_board_action("board_post") && is_board_action("board_read"),
          "is_board_action knows the two board verbs");
    CHECK(!is_board_action("board_admin"), "board_admin is gated separately (host-only)");
    CHECK(!is_board_action("query_realm") && !is_board_action(nullptr) &&
          !is_board_action(""), "is_board_action refuses everything else");
}

// ---------------------------------------------------------------------------
// The PRE-REGISTERED bounds (§3.2)
// ---------------------------------------------------------------------------
// Adopted verbatim into the pre-registration on 2026-09-12, together with the
// frozen shape and the refuse-never-truncate property that earned them. This
// check exists because the COMMENT in board.h rots and a number does not: an
// episode run under different bounds is a different treatment condition, and
// the failure mode is silent — nothing crashes, the arms just stop being
// comparable. If these must change, amend the pre-registration and change them
// here in the same commit, so the two can never disagree unnoticed.
static void test_preregistered_bounds() {
    const BoardConfig d;   // the defaults an unconfigured board would carry
    CHECK(d.max_post_bytes == 2048,
          "PRE-REGISTERED max_post_bytes is 2048");
    CHECK(d.max_posts_per_session_per_flush == 3,
          "PRE-REGISTERED max_posts_per_session_per_flush is 3");
    CHECK(d.max_posts_per_episode == 20000,
          "PRE-REGISTERED max_posts_per_episode is 20000");

    // The property the numbers were adopted FOR, restated where it cannot drift
    // from them: every bound refuses, and a refusal changes nothing else about
    // the board. A bound that truncated would make the pre-registered byte
    // count a claim about what gets SHORTENED rather than what gets REFUSED.
    EpisodeBoard b;
    BoardConfig c;
    c.enabled = true;
    b.configure(c);
    b.begin_episode("ep-prereg");
    const std::string too_long(c.max_post_bytes + 1, 'x');
    BoardPostResult r = b.post_text(too_long, who("s1", 1, "1066.10.14", 1, 1));
    CHECK(!r.ok && r.reason == board_reason::kQuotaExceeded,
          "a post one byte over the pre-registered bound is REFUSED");
    CHECK(b.total_posts() == 0 && b.next_seq() == 0,
          "and the refusal consumed no seq — a bound never half-writes");
    CHECK(b.post_text(std::string(c.max_post_bytes, 'x'),
                      who("s1", 1, "1066.10.14", 1, 1)).ok,
          "a post EXACTLY at the bound is accepted — the bound is inclusive");
}

int main() {
    test_preregistered_bounds();
    test_append_only_and_seq();
    test_attribution_cannot_be_forged();
    test_quotas_are_typed_refusals();
    test_flush_scope();
    test_disabled_and_sealed();
    test_shapes();
    test_arm_gate_from_env();
    printf("test_board: %d checks PASS\n", g_checks);
    return 0;
}
