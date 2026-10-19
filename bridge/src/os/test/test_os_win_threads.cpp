// test_os_win_threads.cpp — os::thread_id / os::set_thread_name on Windows,
// engine-free. thread_id is checked on two threads, so a process id (which
// the main thread's id cannot be told apart from by one sample) would fail;
// the name is read back through GetThreadDescription.

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <thread>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

int main() {
    std::uint64_t t = 0;
    check(os::succeeded(os::thread_id(&t)) && t == GetCurrentThreadId(), "thread_id == GetCurrentThreadId");
    std::uint64_t other = 0, other_direct = 0;
    std::thread th([&] {
        os::thread_id(&other);
        other_direct = GetCurrentThreadId();
    });
    th.join();
    check(other == other_direct && other != t && other != GetCurrentProcessId(),
          "a second thread: its own id, not the main thread's, not the process id");
    check(os::thread_id(nullptr).status == os::Status::failed, "thread_id(null): failed");

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto get = reinterpret_cast<GetDescFn>(
        reinterpret_cast<void*>(GetProcAddress(k32, "GetThreadDescription")));
    const char* name = "ck3-os-test-a-name-longer-than-fifteen-bytes";
    const os::Result r = os::set_thread_name(name);
    if (!GetProcAddress(k32, "SetThreadDescription")) {
        check(r.status == os::Status::failed && r.os_error == ERROR_PROC_NOT_FOUND,
              "no SetThreadDescription export: failed, ERROR_PROC_NOT_FOUND");
    } else {
        check(os::succeeded(r), "set_thread_name: ok");
        PWSTR back = nullptr;
        bool same = false;
        if (get && SUCCEEDED(get(GetCurrentThread(), &back)) && back) {
            char narrow[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, back, -1, narrow, sizeof(narrow), nullptr, nullptr);
            same = std::strcmp(narrow, name) == 0;
            LocalFree(back);
        }
        check(same, "GetThreadDescription reads the whole name back (no 15-byte limit)");
    }
    check(os::set_thread_name(nullptr).status == os::Status::failed, "set_thread_name(null): failed");
    const char bad[] = {char(0xff), char(0xfe), 0};
    check(os::set_thread_name(bad).status == os::Status::failed, "invalid UTF-8: failed");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
