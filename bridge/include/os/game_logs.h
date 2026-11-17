#pragma once
// os/game_logs.h — the game's own debug.log / error.log, found and read for the
// script action layer's read-back (script_action, query_script_log). Lane WINRE.
//
// The files are found by what the GAME has open, never by a path rule the bridge
// re-derives: the game writes them under its user directory, which a -userdir /
// userdir.txt / a moved Documents folder can put anywhere.
//   Linux    the /proc/self/fd walk (readlink of every descriptor), as the
//            v4act code did it (src/os/linux/game_logs.cpp, moved byte for byte)
//   Windows  the process's own handles: NtQuerySystemInformation(extended handle
//            list) filtered to this process, each disk-file handle named by
//            GetFinalPathNameByHandleW (src/os/windows/game_logs.cpp). Read
//            through a handle of the bridge's own, opened shared read+write+delete
//            (the game keeps writing), its size from GetFileSizeEx (a directory
//            entry's size lags an open writer on NTFS).
// Absence is a state: an empty path means "not found", and every reader answers
// -1 / empty for it, never a guessed path.

#include <cstddef>
#include <string>

namespace bridge {
namespace os {

struct GameLogs { std::string debug, error; };

// Which game log a path names, from its tail: 1 = <...>/logs/debug.log,
// 2 = <...>/logs/error.log, 0 = neither. On Windows either separator, and the
// comparison ignores ASCII case (the file system does); elsewhere it is exact.
int game_log_kind(const std::string& path, bool windows_rules);

GameLogs find_game_logs();

// The file's size, -1 when the path is empty or cannot be opened.
long long game_log_size(const std::string& path);

// At most `cap` bytes of the file from `offset` on (the last `cap` bytes when
// more were written since); empty for an empty path, a negative offset or a
// file that cannot be opened.
std::string game_log_read_from(const std::string& path, long long offset, std::size_t cap);

}  // namespace os
}  // namespace bridge
