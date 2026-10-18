# Running the Windows build under Proton (development path)

Goal: get CK3's Windows binary running on a Linux box so the proxy-DLL bridge
can be developed and probed there (ADR 0001, phases P3 and P4). Evidence from
Proton is MEDIUM until repeated on real Windows (limits in the ADR).

**Status: nothing below has been executed by the maintainers.** Every step is
UNVERIFIED unless a note says what was checked. When you run one, edit its tag
and say what you saw. Never share the downloaded files.

You need: Linux x86_64, a Steam account that owns CK3, `steamcmd`, a Proton
(Valve's Proton or Proton-GE), `gdb` (for `gcore`), about 40 GB free.

Variables used below:

```bash
DEPOT=$HOME/ck3-win            # Windows depot goes here, away from your Linux install
PROTON="$HOME/.steam/steam/steamapps/common/Proton - Experimental"
PFX=$HOME/ck3-win-prefix       # Wine prefix (STEAM_COMPAT_DATA_PATH)
```

## 1. Download the Windows depot  [UNVERIFIED]

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

## 2. Inspect it (static)  [UNVERIFIED on ck3.exe; the tool is tested on other PEs]

```bash
python3 tools/inspect_binary.py "$DEPOT/binaries/ck3.exe" --json
```

The path to `ck3.exe` inside the depot is a guess; use `find "$DEPOT" -name ck3.exe`.
File the output as a `binary-report` issue before going further.

## 3. Run under Proton  [UNVERIFIED]

```bash
export STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.steam/steam
export STEAM_COMPAT_DATA_PATH="$PFX"
export SteamAppId=1158310 SteamGameId=1158310
mkdir -p "$PFX"
"$PROTON/proton" run "$DEPOT/binaries/ck3.exe"
```

The two `STEAM_COMPAT_*` variables and the `run` verb were read from the
`proton` script (checked: it exits if `STEAM_COMPAT_DATA_PATH` is unset). Whether
a copy outside a Steam library passes the Steam API check is unknown; the Steam
client must be running. If it fails, record the error. A fallback is a second
Steam library with Proton forced for CK3. `PROTON_LOG=1` writes
`~/steam-1158310.log`.

## 4. Load a proxy DLL  [UNVERIFIED]

Put a DLL named after one of the imports from step 2 (`version.dll` is the
usual) next to `ck3.exe`, and tell Wine to prefer it over the builtin:

```bash
export WINEDLLOVERRIDES="version=n,b"      # native first, builtin as fallback
export WINEDEBUG=+loaddll                  # logs which DLL was actually loaded
"$PROTON/proton" run "$DEPOT/binaries/ck3.exe" 2>&1 | grep -i version.dll
```

Expect a `Loaded ... version.dll ... : native` line. The real bridge DLL does
not exist yet (P2 and P4); until then use a stub that logs and forwards every
export to the system DLL. Do not chain over an existing proxy DLL.

## 5. Take an ELF core of the Wine process  [UNVERIFIED]

The Wine host process is an ordinary Linux process, so `gcore` gives an ELF core
the existing core reader (`re/coredump/core_reader.py` in the research
checkout) can open. Find the process by what it has mapped, not by name:

```bash
PID=$(grep -l 'ck3\.exe' /proc/[0-9]*/maps 2>/dev/null | head -1 | cut -d/ -f3)
STEM=$HOME/ck3-dumps/ck3exe_$(date +%Y%m%d_%H%M%S); mkdir -p "$(dirname "$STEM")"
cp /proc/$PID/maps "$STEM.maps"             # before the dump
gcore -o "$STEM" $PID && mv "$STEM.$PID" "$STEM.core"
```

`gcore` pauses the game for 10 to 30 seconds. Next, the module base: the lowest
address in `$STEM.maps` on a line that ends in `ck3.exe`. Check that `MZ` is
there, then write the sidecar the reader looks for (`<stem>.meta.json`):

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

The reader's ELF-magic base search cannot find a PE, hence the sidecar.
`CoreReader.validate_base` checks for ELF magic and will not apply. The reader lives in the research checkout and is not published yet.

## What to report

Per step: what you ran, what happened, the Proton version (`cat "$PROTON/version"`),
and the Wine log lines for any failure. Edit the step's tag from UNVERIFIED to
`verified <date> on <distro>, <Proton version>`.
