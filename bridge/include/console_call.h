#pragma once
// console_call.h — the engine's console-handler ABI, one definition for every call
// the bridge makes into a console handler (do_script_action -> CMD_EFFECT,
// do_set_doctrine -> CMD_ADD_DOCTRINE).
//
//   SResult* handler(SResult* out, const SCommandData* data)
//     data+0x00  the argument array: one game std::string per argument (stride 0x20)
//     data+0x0C  int32 argument count
//     out+0x00   uint8 success;  out+0x08  the game's std::string message
//
// Both builds take `out` first and `data` second (SysV rdi/rsi, MSVC rcx/rdx: a
// free function returning through the caller's slot). What differs is the
// std::string the game was built with, in both directions:
//   libstdc++ (Linux): char* +0, size +8, SSO buffer +16 (unused when +0 points
//                      elsewhere: the handler only reads an argument).
//   MSVC (Windows):    union { char buf[16]; char* ptr; } +0, size +0x10,
//                      capacity +0x18; the characters are at ptr when capacity
//                      >= 16, else inline at +0. Read off ck3.exe 1.18.3.1: the
//                      argument copy 0x7B3980 (`cmp [rdx+0x18],0x10; jb inline;
//                      mov rdi,[rdx]`, size from [rdx+0x10]) that CMD_EFFECT
//                      (0xDF5340) runs over each argument, and the add_doctrine
//                      parser 0xE0BD70 reading args[0] the same way.
// An argument laid out the libstdc++ way reads, under the MSVC rules, as an
// empty inline string (capacity 0, size 0): the handler would see no script.
// re/findings/windows/re_console_handlers_win.md.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace bridge {
namespace console_call {

// One argument, as the handler's argument array holds it.
struct Arg { std::uint64_t w[4]; };
static_assert(sizeof(Arg) == 0x20, "the handler's argument stride");

// The libstdc++ layout: pointer, length; the buffer words stay zero.
inline Arg arg_libstdcxx(const char* data, std::size_t len) {
    Arg a; std::memset(&a, 0, sizeof a);
    a.w[0] = reinterpret_cast<std::uintptr_t>(data);
    a.w[1] = len;
    return a;
}

// The MSVC layout, always in pointer mode: capacity >= 16 puts the characters
// at w[0] whatever the length (the bridge's string outlives the call; the
// handler copies it and never frees it).
inline Arg arg_msvc(const char* data, std::size_t len) {
    Arg a; std::memset(&a, 0, sizeof a);
    a.w[0] = reinterpret_cast<std::uintptr_t>(data);
    a.w[2] = len;
    a.w[3] = len < 16 ? 16 : len;
    return a;
}

inline Arg arg(const char* data, std::size_t len) {
#ifdef _WIN32
    return arg_msvc(data, len);
#else
    return arg_libstdcxx(data, len);
#endif
}

struct CommandData { Arg* args; std::uint32_t pad_08; std::int32_t arg_count; std::uint8_t pad_rest[48]; };
static_assert(offsetof(CommandData, arg_count) == 0x0C, "argc at +0xC");

// The message word block of SResult, read as the game's own string. Never freed
// by the bridge on Windows: the game allocated it with its own operator new
// (ck3.exe 0x7B4750 -> 0x3CF09B0, a statically linked CRT), so the bridge's
// CRT must not delete it; a message past the 15-byte inline buffer is left
// behind (at most a few hundred bytes per call).
inline std::string message_msvc(const std::uint64_t w[4], std::size_t cap) {
    const std::uint64_t size = w[2], capacity = w[3];
    if (size == 0 || capacity < size) return std::string();
    const char* p = capacity >= 16 ? reinterpret_cast<const char*>(static_cast<std::uintptr_t>(w[0]))
                                   : reinterpret_cast<const char*>(w);
    if (!p) return std::string();
    return std::string(p, size < cap ? static_cast<std::size_t>(size) : cap);
}
inline std::string message_libstdcxx(const std::uint64_t w[4], std::size_t cap) {
    const std::uint64_t size = w[1];
    const char* p = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(w[0]));
    if (size == 0 || !p) return std::string();
    return std::string(p, size < cap ? static_cast<std::size_t>(size) : cap);
}

#ifdef _WIN32
// Raw words: the bridge's MSVC std::string is the same layout, but its
// destructor would free the game's allocation with the bridge's CRT.
struct Result {
    std::uint8_t success; std::uint8_t pad[7]; std::uint64_t msg[4];
    Result() { std::memset(this, 0, sizeof *this); }
    std::string message(std::size_t cap) const { return message_msvc(msg, cap); }
};
static_assert(offsetof(Result, msg) == 8, "the message at +8");
#else
// Linux keeps the shape a live game proved: a libstdc++
// std::string the game fills and the bridge's destructor frees (one libstdc++
// and one allocator in the process).
struct Result {
    std::uint8_t success; std::uint8_t pad[7]; std::string msg;
    Result() : success(0), pad{} {}
    std::string message(std::size_t cap) const { return msg.substr(0, msg.size() < cap ? msg.size() : cap); }
};
#endif


}  // namespace console_call
}  // namespace bridge
