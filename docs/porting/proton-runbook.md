# Running the Windows build under Proton (development path)

Goal: get CK3's Windows binary running on a Linux box so the proxy-DLL bridge
can be developed and probed there (ADR 0001, phases P3 and P4). Evidence from
Proton is MEDIUM until repeated on real Windows (limits in the ADR).

**Status: steps 3, 4, 5 and 6 were run on 2026-10-19 on one Linux machine with
no Steam client installed (step 4 with the bridge DLL, on the pinned 1.18.3.1
depot and on 1.20.1.1; step 6 with the 867 observer save);** their notes say what was seen and
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

## 4. Load the bridge DLL  [verified 2026-10-19 on Fedora 44, Proton experimental-11.0-20261014: match on 1.18.3.1, REFUSED on 1.20.1.1]

The bridge DLL is `ck3_bridge_win`, built as `version.dll` (`ck3.exe` imports
`VERSION.dll` and `WINMM.dll`; it calls `GetFileVersionInfoA`,
`GetFileVersionInfoSizeA` and `VerQueryValueA` from it). Build it with the
clang-cl/xwin toolchain (or MSVC) and put it next to `ck3.exe`, telling Wine to
prefer it over the builtin:

```bash
cmake -S bridge -B bridge/build/win -G Ninja -DCK3_PLATFORM=windows \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/bridge/cmake/toolchains/clang-cl-xwin.cmake
cmake --build bridge/build/win --target ck3_bridge_win     # -> bridge/build/win/version.dll
cp bridge/build/win/version.dll "$DEPOT/binaries/"   # refuse if a version.dll is already there
export WINEDLLOVERRIDES="version=n,b"      # native first, builtin as fallback
export WINEDEBUG=+loaddll                  # logs which DLL was actually loaded
# start as in step 3, then:
grep -i 'loaddll.*version.dll' "$PROTON_LOG_DIR/steam-1158310.log"
cat "$PFX/pfx/drive_c/users/steamuser/AppData/Local/ck3agent/bridge.log"
```

With `PROTON_LOG=1` the Wine debug output goes to the log file, not to the
terminal. Seen in the log, thread `0144` being the game's loader:

```
0144:trace:loaddll:build_module Loaded L"Z:\\mnt\\data\\ck3-win-1183\\binaries\\VERSION.dll" at 00006FFFFB820000: native
0170:trace:loaddll:build_module Loaded L"C:\\windows\\system32\\version.dll" at 00006FFFFDCE0000: builtin
```

The second line is the DLL's init thread loading the real DLL by full path: a
different module at a different base, so the full-path load does not hand the
proxy back to itself. Without the DLL, thread `0144` loads the builtin instead.

What the DLL does (`bridge/src/os/windows/dll_main.cpp`, `proxy.cpp`):
`DllMain` records the module and starts one thread, nothing else. That thread
resolves the forwards to `<System32>\version.dll` (a forward called earlier
resolves on its own thread, lock-free), checks that the host is `ck3.exe`,
then runs the identity gate: the CodeView GUID+age and `SizeOfImage` of the
running image against the pinned 1.18.3.1 constants
(`bridge/src/os/windows/pinned_identity.h`; the file SHA-256 is the key only
when no CodeView record is readable, and is otherwise reported as a second
witness). It logs to `%LOCALAPPDATA%\ck3agent\bridge.log` (the instance paths;
`%TEMP%\ck3_bridge_win.log` if they are refused). On `match`, a build with the
bridge core (`CK3_WIN_BRIDGE_CORE`, on by default since P5) starts the bridge
observe-only: no hooks, an IPC listener on TCP loopback (§7). The P4 runs below
were made before that, with nothing started on `match`. On any other build it logs
`REFUSED` with both identities, starts nothing, and keeps forwarding; the game
runs normally. The pinned 1.18.3.1 depot:

```
[ck3_bridge_win] forward real=C:\windows\system32\version.dll module=00006FFFFDCE0000 err=0 resolved=16/17 missing=[GetFileVersionInfoByHandle] first=init_thread
[ck3_bridge_win] host exe=Z:\mnt\data\ck3-win-1183\binaries\ck3.exe is_game=1
[ck3_bridge_win] identity observed format=pe_codeview_rsds key=3e26a827c47a4fb0a8633288849287c800000001 size_of_image=94806016 sha256=2f1358c0...2fccb0f (416ms)
[ck3_bridge_win] identity match key=codeview+size_of_image sha256_witness=agreed: nothing started (ADR P4: no hooks, no IPC listener)
[ck3_bridge_win] calls after 90000ms: GetFileVersionInfoSizeW=1 GetFileVersionInfoW=1 VerQueryValueW=4
```

The 1.20.1.1 depot (public branch):

```
[ck3_bridge_win] identity observed format=pe_codeview_rsds key=0df59548663d4c98a61c878362242e2b00000001 size_of_image=102600704 sha256=12cbd0d6...8a008939 (447ms)
[ck3_bridge_win] identity REFUSED check=mismatch key=codeview+size_of_image why=CodeView GUID+age and SizeOfImage both differ from the pin sha256_witness=disagreed: nothing started, forwarding only; the game keeps running
```

Both reached the main menu with the DLL in place, and neither game process
had a TCP or UDP listener (`ss -ltnup`; its only listening socket is the
NVIDIA driver's `@cuda-uvmfd-...` abstract socket, present without the DLL
too). The export list is generated from Wine's builtin `version.dll` (16
names) plus `GetFileVersionInfoByHandle`, which Windows' own exports and Wine's
lacks (`bridge/src/os/windows/version_exports_extra.h`); regenerate from a real
System32 copy before the Windows run (P7). Evidence and timings:
`re/findings/windows/re_p4_proxy_gate.md` in the research checkout. Do not
chain over an existing proxy DLL.

The P3 load-check stub (`bridge/src/os/win/proxy_stub/`, forwards only, logs to
`C:\ck3_proxy_stub.log`) is kept as a diagnostic for a proxy load that fails
silently.

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

## 6. Load a save  [verified 2026-10-19 on Fedora 44, Proton experimental-11.0-20261014: 1.18.3.1, the 867 observer save]

The Windows game keeps its user data inside the prefix:
`$PFX/pfx/drive_c/users/steamuser/Documents/Paradox Interactive/Crusader Kings III/`.
Put the save in `save games/` there, write `continue_game.json` beside it with
`"title": "<save name without .ck3>"`, and start the game with `--continuelastsave`
(`proton run ck3.exe --continuelastsave`). It loads straight into the save. An
observer save stops at the character-select lobby, with the clock not running.
Remove both files afterwards.

- **Without a Steam client the game owns no DLC.** A save that lists DLCs (a
  `dlcs={...}` line in its `meta_data`) fails with "Failed to Load Savegame", and
  `error.log` names each missing DLC. Removing that list from both copies of
  `meta_data` (the text block after the header and the top of the zipped
  `gamestate`) lets it load. The research checkout's
  `re/scripts/windows/strip_save_dlcs.py` does exactly that. Not tested with the
  Steam client running.
- Seen: `debug.log` `End loading of history` at about +90 s. The logs then go
  quiet, and the lobby showed 1 January 867. The ELF core (§5) taken there was
  16.6 GB apparent and 8.6 GB on disk.
- For a comparison with the Linux build, load **the same file** in a plain Linux
  game (no bridge, or `CK3_SUPPRESS_AUTO_UNPAUSE=1`). A bridge-loaded Linux game
  runs its clock: one reached 869.07.12 while sitting in the lobby, and the
  Windows game stayed at 867.01.01. Check the date in both cores before comparing
  anything.

## 7. Talk to the bridge (observe only)  [verified 2026-10-19 on Fedora 44, Proton experimental-11.0-20261014: 1.18.3.1, the 867 save, every query_* answered]

With the bridge core in `version.dll` (§4), a `match` starts the bridge with no
hooks and an IPC listener on TCP loopback. It writes the endpoint into the
per-user state directory, where a client finds the address and the token:

```bash
E="$PFX/pfx/drive_c/users/steamuser/AppData/Local/ck3agent/endpoint.json"
cat "$E"     # {"transport":"tcp_loopback","endpoint":"127.0.0.1:<port>","token":"<64 hex>","pid":<n>}
```

The client's first line must be `{"auth":"<token>"}`; the bridge answers
`{"type":"auth_ok"}` (or `auth_refused` and closes). After that the protocol is
the Linux one (NDJSON). `CK3_BRIDGE_PORT` fixes the port; otherwise the system
picks a free one. The research checkout's `re/scripts/windows/parity_probe.py
--endpoint "$E" --plan plan.json --out windows.jsonl` does the handshake and
records every reply; `node/p5-win-v1.sh` is the whole run (stage the save,
start, wait, probe, kill the recorded pid, remove the DLL and the endpoint).

Seen in `bridge.log` at the menu or in the lobby:

```
[CK3-Bridge] IPC listening on 127.0.0.1:44835 (tcp_loopback); endpoint C:\users\steamuser\AppData\Local\ck3agent\endpoint.json
[CK3-Bridge] Initialization complete (Windows, observe-only)
```

- Since P6 the bridge is no longer observe-only (§8); the offset validation
  checks the Windows names and passes, and a `DEGRADED MODE` verdict now means
  what it means on Linux: IPC only, no hooks.
- A read the Windows header cannot place (a poisoned constant) fails cleanly; a
  degraded reply logs `poison_index=<i>`, which names the constant in the
  build's `win_offsets/report.json`.
- The game can exit on its own when the GPU is short of memory
  (`vkAllocateMemory` failing in the Proton log); the node's headless compositor
  has held 4.3 of 6 GiB with no game running. Check `nvidia-smi` first.

Parity with the Linux bridge on the same save, the method and the results:
`re/findings/windows/re_p5_observe_parity.md` in the research checkout.

## 8. Steer through the bridge (the AI-tick hook)  [verified 2026-10-19 on Fedora 44, Proton experimental-11.0-20261014: 1.18.3.1, the 867 save, PROTON_USE_WINED3D=1]

On `match` the bridge installs the AI-tick hook once the save has loaded (a swap of
one slot in CAIManager's +8 vtable) and from then on works as on Linux: a client
that handshakes with `"mode":"control"` can claim a character and steer it
(personality axes, AI subsystem toggles, the character's AI, pause and speed).
Seen in `bridge.log`:

```
[CK3-Bridge] Tick hook installed (Windows): +8 vtable[1] (orig=000000014175AB80 -> on_tick_internal)
[CK3-Bridge] BUILD-B: First tick received — DORMANT -> ACTIVE
[CK3-Bridge] Watchdog: Windows thread interrupt (tid=...; armed per calling thread)
```

The first tick comes with the first unpause: the bridge unpauses a loaded game by
itself unless `CK3_SUPPRESS_AUTO_UNPAUSE=1` is set (the parity runs set it, so the
clock moves only when a plan step moves it). Not on Windows yet: the observers that
Linux builds on inline hooks (war score, war and scheme creation, marriages,
council tasks, the CB selector, the off-goal veto), CB and interaction validity
(they refuse or report the engine call failed), and localized character names
(`name_pending` stays true).

**Renderer.** On the node the game died at the end of loading under DXVK, in
`vkAllocateMemory` (the GPU short of memory; the bridge not involved, a run without
the hook died the same way). `PROTON_USE_WINED3D=1` (OpenGL) loaded every time;
`DXVK_CONFIG="dxgi.maxDeviceMemory = 1024"` did not help. `node/p6-win-v1.sh` in the
research checkout runs the parity plan and a steering plan; P6 results:
`re/findings/windows/re_p6_hooks_steering.md`.

## What to report

Per step: what you ran, what happened, the Proton version (`cat "$PROTON/version"`),
and the Wine log lines for any failure. Edit the step's tag from UNVERIFIED to
`verified <date> on <distro>, <Proton version>`.
