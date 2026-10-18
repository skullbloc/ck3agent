// Regression test for ck-8xzy: json_get_string must not clobber earlier
// returned pointers when called multiple times for different string params.
//
// Build: handled by CMake target `test_json_helpers` (BUILD_BRIDGE_TESTS=ON).
// Run: ./test_json_helpers  (exits non-zero on failure)

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rapidjson/document.h>

// Link against the real implementation from dispatch_router.cpp by pulling
// the function in via its declaration. The test target compiles
// dispatch_router.cpp standalone with a stub for g_logfile / safe_read etc.
namespace bridge {
const char* json_get_string(const char* json, const char* key);
}

using bridge::json_get_string;

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d): %s\n", #cond, __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

// Scenario from bead: do_query_memory_probe reads base, address, offset,
// global_name back-to-back. Pre-fix, the single thread_local buffer meant all
// four pointers aliased the last value.
static void test_memory_probe_four_strings() {
    const char* json =
        R"({"params":{"base":"global","address":"0xDEADBEEF",)"
        R"("offset":"0x100","global_name":"GAME_STATE_ROOT"}})";

    const char* base = json_get_string(json, "base");
    const char* address = json_get_string(json, "address");
    const char* offset = json_get_string(json, "offset");
    const char* global_name = json_get_string(json, "global_name");

    CHECK(base && std::strcmp(base, "global") == 0,
          "base should still read 'global' after later calls");
    CHECK(address && std::strcmp(address, "0xDEADBEEF") == 0, "address mismatch");
    CHECK(offset && std::strcmp(offset, "0x100") == 0, "offset mismatch");
    CHECK(global_name && std::strcmp(global_name, "GAME_STATE_ROOT") == 0,
          "global_name mismatch");

    CHECK(base != address && address != offset && offset != global_name,
          "rotating pool must return distinct buffer addresses");
}

// Scenario from bead: do_query_resolve_global reads name + address.
static void test_resolve_global_two_strings() {
    const char* json =
        R"({"params":{"name":"PLAYER_CHAR_ID","address":"0xABCDEF"}})";

    const char* name = json_get_string(json, "name");
    const char* address = json_get_string(json, "address");

    CHECK(name && std::strcmp(name, "PLAYER_CHAR_ID") == 0,
          "name clobbered by later address call");
    CHECK(address && std::strcmp(address, "0xABCDEF") == 0, "address mismatch");
}

// Top-level key (not under "params") must also work — dispatch_router.cpp:1424
// reads "type" from the top level.
static void test_top_level_key() {
    const char* json = R"({"type":"query","id":"42"})";
    const char* type = json_get_string(json, "type");
    const char* id = json_get_string(json, "id");
    CHECK(type && std::strcmp(type, "query") == 0, "top-level type wrong");
    CHECK(id && std::strcmp(id, "42") == 0, "top-level id wrong");
}

// Missing keys return nullptr without touching earlier results.
static void test_missing_key_returns_null() {
    const char* json = R"({"params":{"foo":"bar"}})";
    const char* foo = json_get_string(json, "foo");
    const char* missing = json_get_string(json, "nope");
    CHECK(foo && std::strcmp(foo, "bar") == 0, "foo lost");
    CHECK(missing == nullptr, "missing key should return nullptr");
}

// String longer than BUF_SIZE (256) gets truncated without overflow.
static void test_long_string_truncation() {
    std::string big(400, 'x');
    std::string j = std::string(R"({"params":{"k":")") + big + R"("}})";
    const char* v = json_get_string(j.c_str(), "k");
    CHECK(v != nullptr, "long-string call returned null");
    CHECK(std::strlen(v) == 255, "truncated length should be BUF_SIZE-1");
}

// Thread-local pools must be independent: buffers from thread A must not
// alias buffers from thread B.
static void test_thread_local_isolation() {
    const char* json = R"({"params":{"a":"AAA","b":"BBB"}})";
    const char* ptr_a_thread1 = nullptr;
    const char* ptr_a_thread2 = nullptr;

    std::thread t1([&]() {
        ptr_a_thread1 = json_get_string(json, "a");
    });
    t1.join();
    std::thread t2([&]() {
        ptr_a_thread2 = json_get_string(json, "a");
    });
    t2.join();

    // Different threads should have distinct thread_local pools. We can't
    // inspect that without racing; instead verify both calls returned the
    // expected content.
    CHECK(ptr_a_thread1 && std::strcmp(ptr_a_thread1, "AAA") == 0,
          "thread 1 got wrong value");
    CHECK(ptr_a_thread2 && std::strcmp(ptr_a_thread2, "AAA") == 0,
          "thread 2 got wrong value");
}

// Stress: 20 back-to-back reads. Pool is 16, so after 16 calls the pool
// wraps and earlier pointers become stale. This documents the bound rather
// than claiming infinite-retention works.
static void test_pool_wrap_bound() {
    std::string j = R"({"params":{)";
    for (int i = 0; i < 20; i++) {
        if (i) j += ",";
        j += "\"k"; j += std::to_string(i); j += "\":\"v";
        j += std::to_string(i); j += "\"";
    }
    j += "}}";

    std::vector<const char*> ptrs;
    for (int i = 0; i < 20; i++) {
        std::string key = "k" + std::to_string(i);
        ptrs.push_back(json_get_string(j.c_str(), key.c_str()));
    }
    // Last 16 calls (indices 4..19) should still hold their distinct values.
    for (int i = 4; i < 20; i++) {
        std::string want = "v" + std::to_string(i);
        CHECK(ptrs[i] && std::strcmp(ptrs[i], want.c_str()) == 0,
              "wrap window pointer lost its value");
    }
}

int main() {
    test_memory_probe_four_strings();
    test_resolve_global_two_strings();
    test_top_level_key();
    test_missing_key_returns_null();
    test_long_string_truncation();
    test_thread_local_isolation();
    test_pool_wrap_bound();

    if (failures == 0) {
        std::printf("test_json_helpers: OK (all checks passed)\n");
        return 0;
    }
    std::fprintf(stderr, "test_json_helpers: %d failure(s)\n", failures);
    return 1;
}
