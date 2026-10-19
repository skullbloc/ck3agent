# Running the Windows build under Proton (development path)

Goal: get CK3's Windows binary running on a Linux box so the proxy-DLL bridge
can be developed and probed there (ADR 0001, phases P3 and P4). Evidence from
Proton is MEDIUM until repeated on real Windows (limits in the ADR).

**Status: steps 3, 4 and 5 were run once, on 2026-10-19, on one Linux
machine with no Steam client installed;** their notes say what was seen and
where the commands had to change. Steps 1 and 2 are still UNVERIFIED here.
When you run a step, edit its tag and say what you saw. Never share the
downloaded files.

You need: Linux x86_64, a Steam account that owns CK3, `steamcmd`, a Proton
(Valve's Proton or Proton-GE), `gdb` (for `gcore`), about 40 GB free.

Variables used below:

```bash
DEPOT=$HOME/ck3-win            # Windows depot goes here, away from your Linux install
PROTON="$HOME/.steam/steam/steamapps/common/Proton - Experimental"
PFX=$HOME/ck3-win-prefix       # Wine prefix (STEAM_COMPAT_DATA_PATH)
STEAMCMD_DIR=$HOME/steamcmd    # only for the no-Steam-client workaround in step 3
```

## 1. Download the Windows depot  [VERIFIED 2026-10-19: see the branch note]

```bash
steamcmd +@sSteamCmdForcePlatformType windows \
         +force_install_dir "$DEPOT" +login <your_steam_user> \
         +app_update 1158310 -beta <branch> validate +quit
```

- `@sSteamCmdForcePlatformType` must come before `+login`.
- `<branch>` is the Betas branch that serves 1.18.3.1 (the pin). List branches:
  `steamcmd +login <user> +app_info_print 1158310 +quit` and read `depots` then
  `branches`. If no branch serves it, stop and report: that is a kill trigger.
- Type the password when prompted; do not put it on the command line.
- For macOS use `macos` instead of `windows`.

**Branch note (verified 2026-10-19).** The public branch no longer serves the
pinned build: a plain `app_update 1158310` fetched 1.20.1.1 (Crozier). The
rollback branch is named after the minor version and serves the hotfix:

```bash
steamcmd +@sSteamCmdForcePlatformType windows +force_install_dir "$DEPOT" \
  +login <user> +app_update 1158310 -beta 1.18.3 validate +quit
```

Check `"$DEPOT/launcher/launcher-settings.json"` (`rawVersion`) and
`"$DEPOT/titus_branch.txt"` (`release/1.18.3`) before anything else; the
Linux install carries the same two files. `steamcmd +app_info_print 1158310`
lists every branch under `depots.branches` (1.17.0 through 1.20.0 at the time
of writing; no password required for the version branches).

## 2. Inspect it (static)  [UNVERIFIED on ck3.exe; the tool is tested on other PEs]

```bash
python3 tools/inspect_binary.py "$DEPOT/binaries/ck3.exe" --json
```

The path to `ck3.exe` inside the depot is a guess; use `find "$DEPOT" -name ck3.exe`.
File the output as a `binary-report` issue before going further.

## 3. Run under Proton  [verified 2026-10-19 on Fedora 44, Proton experimental-11.0-20261014, with no Steam client installed; see notes]

```bash
export STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.steam/steam
export STEAM_COMPAT_DATA_PATH="$PFX"
export SteamAppId=1158310 SteamGameId=1158310
export PROTON_LOG=1 PROTON_LOG_DIR="$HOME/ck3-win-logs"   # log: $PROTON_LOG_DIR/steam-1158310.log
mkdir -p "$PFX" "$PROTON_LOG_DIR"
cd "$DEPOT/binaries"
setsid nohup "$PROTON/proton" run "$DEPOT/binaries/ck3.exe" > "$PROTON_LOG_DIR/proton.out" 2>&1 < /dev/null &
```

The two `STEAM_COMPAT_*` variables and the `run` verb were read from the
`proton` script (it exits if `STEAM_COMPAT_DATA_PATH` is unset). Without
`PROTON_LOG_DIR` the log is `~/steam-1158310.log`. Start it detached
(`setsid nohup`): the game must not die with your shell. It was run on an X11
display (GNOME's Xwayland, `env -u WAYLAND_DISPLAY DISPLAY=:0`).

What happened, run by run:

- **No Steam client on the machine: `proton run` hangs before the game.**
  Proton's own `steam.exe` loads `lsteamclient`, which looks for
  `$HOME/.steam/sdk64/steamclient.so` (string read from
  `files/lib/wine/x86_64-unix/lsteamclient.so`). Without it the log says
  `err:steamclient:steamclient_init unable to load native steamclient library`,
  then `_wassert (L"!status",L"../src-lsteamclient/steamclient_main.c",375)`,
  a "Wine C++ Runtime Library" dialog opens and `ck3.exe` never starts. With a
  Steam client installed that file exists; that case was **not** tested.
- **Workaround that reached the menu** (still no Steam client): give the
  `proton` command a `HOME` whose `.steam/sdk64/steamclient.so` is steamcmd's
  `linux64/steamclient.so`; your real `HOME` is not touched:
  ```bash
  H=$HOME/ck3-win-home; mkdir -p "$H/.steam/sdk64"
  ln -sfn "$STEAMCMD_DIR/linux64/steamclient.so" "$H/.steam/sdk64/steamclient.so"
  export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAMCMD_DIR"   # there is no $HOME/.steam/steam
  HOME="$H" setsid nohup "$PROTON/proton" run "$DEPOT/binaries/ck3.exe" ... &
  ```
  The log then shows `err:steamclient:steamclient_init_registry Failed to
  connect to Steam` (not fatal), `steam.exe` starts the game, and the game's
  own Steam check fails without stopping it:
  `[S_API] SteamAPI_Init(): Sys_LoadModule failed to load: C:\Program Files (x86)\Steam\steamclient64.dll`.
  `error.log` gets one `dlc.cpp:881: Could not find item in store backend.`
  per DLC. So a copy outside a Steam library is not refused at a Steam API
  check; it just runs without Steam.
- **Main menu: yes, in 85 s** from the `proton` command (game log open at
  +2 s; `frontendmainview.cpp: Held the main menu` at +85 s), on a prefix the
  first run had created (about 325 MB). A second run with the step 4 proxy:
  90 s. The menu shows `Game Version: Crozier (1.20.1.1)` and the user
  agreement dialog of a fresh profile.
- **Paradox launcher: not involved.** `ck3.exe` is started directly;
  `launcher/dowser.exe` is never run.
- **GPU:** DXVK v3.1.1 on the NVIDIA GPU (GTX 1660 Ti, driver 595.80; llvmpipe
  skipped as a software driver), D3D11 feature level 11_0. At the menu
  `nvidia-smi` showed `ck3.exe` with 1.1 to 1.3 GB of VRAM; resident memory
  5.4 to 6.2 GB.
- **Screenshots:** a root-window grab fails under Xwayland (BadMatch); grab
  the game window by id: `xdotool search --all --pid $PID --name 'Crusader Kings III'`,
  then `gst-launch-1.0 ximagesrc xid=<id> num-buffers=1 ! videoconvert ! pngenc ! filesink location=menu.png`.
- **Version:** a depot fetched without `-beta` is the public branch (1.20.1.1
  on that date), not the 1.18.3.1 pin. Fetch the pinned branch (step 1) before
  any comparison with the Linux reference.
- **Stopping it:** `SIGKILL` the game's pid only (step 5 says how to find it);
  `steam.exe`, `wineserver` and the prefix's service processes then exit on
  their own within about 8 s.
- The fallback "a second Steam library with Proton forced for CK3" needs the
  Steam client and was not tried.

## 4. Load a proxy DLL  [verified 2026-10-19 on Fedora 44, Proton experimental-11.0-20261014]

Put a DLL named after one of the imports from step 2 (`version.dll`; `ck3.exe`
imports `VERSION.dll` and `WINMM.dll`) next to `ck3.exe`, and tell Wine to
prefer it over the builtin:

```bash
cp version.dll "$DEPOT/binaries/"          # refuse if a version.dll is already there
export WINEDLLOVERRIDES="version=n,b"      # native first, builtin as fallback
export WINEDEBUG=+loaddll                  # logs which DLL was actually loaded
# start as in step 3, then:
grep -i 'version.dll' "$PROTON_LOG_DIR/steam-1158310.log"
```

With `PROTON_LOG=1` the Wine debug output goes to the log file, not to the
terminal; without it, it goes to stderr. Seen in the log of the `ck3.exe`
process:

```
trace:loaddll:build_module Loaded L"Z:\\...\\binaries\\VERSION.dll" at 00006FFFFB7B0000: native
trace:loaddll:build_module Loaded L"C:\\windows\\system32\\version.dll" at 00006FFFFDCE0000: builtin
```

The second line is the proxy loading the real DLL by full path; it is a
different module at a different base, so a full-path load does not hand the
proxy back to itself. The other `builtin` version.dll lines in the log belong
to Proton's `steam.exe` and helper processes.

The stub used is `bridge/src/os/win/proxy_stub/` (source; build with
`build.sh`, `x86_64-w64-mingw32-gcc` or llvm-mingw's clang). It forwards every
export of the real DLL through a pointer resolved on attach and appends one line
per attach to `C:\ck3_proxy_stub.log` (`$PFX/pfx/drive_c/` under Proton):

```
attach 2026-10-19T01:38:43Z pid=356 tid=360 self=00006ffffb7b0000 (Z:\...\binaries\VERSION.dll) exe=Z:\...\binaries\ck3.exe real=00006ffffdce0000 (C:\windows\system32\version.dll) err=0 resolved=16/16 missing=[]
```

The game reached the main menu with the proxy in place. Its per-export call
counters, read from the live process (`/proc/<pid>/mem` at the counters'
addresses), showed the forwards in use by then: `GetFileVersionInfoSizeW` 1,
`GetFileVersionInfoW` 1, `VerQueryValueW` 4. The export list is generated from
Wine's `version.dll` (16 names); Windows' own also exports
`GetFileVersionInfoByHandle`, so regenerate the list from a real System32 copy
before the Windows run (P7). `LoadLibraryW` from `DllMain` works under Wine's
loader; that says nothing about Windows' loader lock. The real bridge DLL
does not exist yet (P2 and P4). Do not chain over an existing proxy DLL.

## 5. Take an ELF core of the Wine process  [verified 2026-10-19 on Fedora 44, Proton experimental-11.0-20261014, gdb 17.2]

The Wine host process is an ordinary Linux process, so `gcore` gives an ELF core
the existing core reader (`re/coredump/core_reader.py` in the research
checkout) can open. Find the process by what it has mapped, not by name, and
only once the game window is up: in its first seconds Proton's `steam.exe`
maps `ck3.exe` too, and the first match was `steam.exe`. Wine starts each
Windows process in its own session with parent pid 1, so it is not a child of
the `proton` command either. The finder below also requires argv[0] to be
`ck3.exe`; sampled every 2 s over a launch, the one-line `grep -l ... | head -1`
picked `steam.exe` at about +4 s, this one only ever picked the game. Check
what you found before using it:

```bash
PID=$(grep -l 'binaries/ck3\.exe' /proc/[0-9]*/maps 2>/dev/null | cut -d/ -f3 | while read p; do
        tr '\0' '\n' < /proc/$p/cmdline | head -1 | grep -qi 'ck3\.exe$' && echo $p; done | head -1)
tr '\0' ' ' < /proc/$PID/cmdline; echo          # expect Z:\...\binaries\ck3.exe
STEM=$HOME/ck3-dumps/ck3exe_$(date +%Y%m%d_%H%M%S); mkdir -p "$(dirname "$STEM")"
cp /proc/$PID/maps "$STEM.maps"             # before the dump
gcore -o "$STEM" $PID && mv "$STEM.$PID" "$STEM.core"
```

`gcore` attaches with ptrace (`kernel.yama.ptrace_scope` 0, or root). It took
18 s at the main menu; the game kept running. The core was 14 GB apparent and
5.8 GB on disk (sparse), 1,403 `PT_LOAD` segments, for a 6.2 GB resident
process; put it on a disk with room. gdb's only warning was a failed read of
the vsyscall page (`0xffffffffff600000`), which is harmless.

Next, the module base: the lowest address in `$STEM.maps` on a line that ends
in `ck3.exe`. Only two lines name the file: the header page at the base and one
page further in. Wine copied the sections into anonymous memory (the file
alignment, 0x200, is below the page size), and gdb's default
`coredump_filter` (0x33) keeps anonymous memory, so the whole image is in the
core. Observed base: `0x140000000`, the PE's own `ImageBase`; Wine kept the
preferred base although the image allows ASLR, so here RVA = Ghidra address
minus `0x140000000`. Check that `MZ` is there, then write the sidecar the
reader looks for (`<stem>.meta.json`):

```bash
echo '{"base_addr": "0x<BASE>", "proc_maps_file": "<STEM>.maps"}' > "$STEM.meta.json"
```

```python
import sys; sys.path.insert(0, "re/coredump")
from core_reader import CoreReader
r = CoreReader(STEM + ".core")             # base_addr comes from the sidecar
assert r.read_bytes(r.base_addr, 2) == b"MZ"
r.read_u64(r.resolve(0x1234))              # resolve(rva) = base + RVA
```

Seen: `MZ` at the base, `PE\0\0` at `e_lfanew` 0x158, `ImageBase` and
`SizeOfImage` in the in-memory header equal to the file's, and
`resolve(0x1000)` (start of `.text`) returns the same 16 bytes as the file at
`.text`'s raw offset. The reader's ELF-magic base search cannot find a PE,
hence the sidecar. `CoreReader.validate_base` checks for ELF magic and will
not apply. The reader lives in the research checkout and is not published yet.

## What to report

Per step: what you ran, what happened, the Proton version (`cat "$PROTON/version"`),
and the Wine log lines for any failure. Edit the step's tag from UNVERIFIED to
`verified <date> on <distro>, <Proton version>`.
