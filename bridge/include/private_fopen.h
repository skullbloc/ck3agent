// private_fopen.h — rc2 B1: the one way a bridge TU opens a file it writes.
//
// SECURITY.md says bridge files are 0600 and opened without following
// symlinks. This is what makes that true for files opened by path:
// instance_fopen_private (instance_paths.cpp) opens O_NOFOLLOW|O_CLOEXEC,
// regular files only, created 0600 (0644 legacy) and tightened to that mode
// on a write open.
//
// Weak: several host targets compile session.cpp / board_persist.cpp without
// instance_paths.cpp; they get plain fopen. The .so always links it.
// test/check_private_file_opens.py fails on any other write-mode fopen,
// O_CREAT without O_NOFOLLOW, or ofstream under bridge/src and bridge/include.
#pragma once

#include <cstdio>

namespace bridge {

FILE* instance_fopen_private(const char* path, const char* mode) __attribute__((weak));

inline FILE* private_fopen(const char* path, const char* mode) {
    return instance_fopen_private ? instance_fopen_private(path, mode)
                                  : std::fopen(path, mode);
}

}  // namespace bridge
