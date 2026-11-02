# P7: validating the Windows bridge on real Windows (runbook)

ADR 0001 phase P7: "P4-P6 repeated on Windows; installer tested", on a rented GPU
VM. Everything before P7 ran under Proton on Linux (`proton-runbook.md`). This
runbook is the plan for the first real-Windows run: what the owner provides, what
the lane runs, and what each step must show. It has not been executed yet.

## What only real Windows can tell us

Proton is a faithful Windows, not Windows. These are the questions P7 exists for:

- the proxy `version.dll` loading from the game directory under the real loader
  (search order, `KnownDLLs`, the loader lock), and its forwards to the real
  `System32\version.dll` (`GetFileVersionInfoByHandle` never resolved under Wine);
- Defender and SmartScreen on an unsigned DLL and zip (P7 ships them signed);
- the vectored exception handler, `SuspendThread`/`SetThreadContext` (the watchdog)
  and RWX trampoline pages under the real kernel (CFG is off in this `ck3.exe`);
- the renderer: native DX11 on a real driver (under Proton the node needed
  `PROTON_USE_WINED3D=1`);
- `install.ps1` under Windows PowerShell 5.1 and PowerShell 7 (tested only under
  PowerShell 7 for Linux so far: `installer/windows/test_install_ps1.sh`).

## Before the VM

- **The Windows CI leg on real MSVC.** `platform-windows` in `.github/workflows/ci.yml`
  (cells, the DLL, the engine-free tests, the installer scripts parsed under
  PowerShell 7 and 5.1) has only ever run with the cross toolchain; it runs on real
  MSVC when the owner syncs the public repo. It stays `continue-on-error` until it is
  green there twice.
- **The Windows offsets in the public export** (owner's decision 2026-10-19: decided).
  The export ships Windows offset headers generated from the research inputs
  (`re/offsets/windows/`), so a public build is the full bridge and a P7 run can use
  the release zip's DLL.

## What the owner provides (the lane cannot)

1. **An AWS account with GPU quota.** A `g4dn.xlarge` (1 NVIDIA T4, 4 vCPU, 16 GiB)
   is enough for CK3. New accounts usually have a quota of 0 for "Running On-Demand
   G and VT instances": request at least 4 vCPU in the region you will use (the
   request can take a day). DigitalOcean has no Windows images (ADR).
2. **The machine.** Windows Server 2022 with the NVIDIA gaming/GRID driver (AWS
   publishes driver instructions per AMI), 150 GiB gp3. Inbound: RDP (and OpenSSH,
   step 3) from your own address only.
3. **Cost.** On-demand Windows pricing for `g4dn.xlarge` is on the order of one
   US dollar per hour plus storage; check the current price for your region before
   starting. A P7 run is planned as two sessions of about four hours; stop (not
   terminate) the instance between sessions and terminate it at the end.
4. **Steam.** Log in with your account in the VM (2FA is yours), install Crusader
   Kings III, and select the beta branch `1.18.3` (Properties > Betas): the bridge
   supports exactly that build. No DLC is needed (the reference save is DLC-free).
5. **Access for the lane**, one of: an OpenSSH server in the VM with a key the lane
   holds, or you run the scripts below yourself and send back the output folders.
6. **The code-signing certificate**, if the zip is to ship: `package_windows.py
   --require-signed` refuses an unsigned DLL.

## What the lane runs (in order; each step names its pass condition)

Inputs the lane brings: the release zip (`package_windows.py`), the reference save
`p3probe_867_nodlc.ck3`, the parity plan, the steering plan and the dispatch plan
(`p6d-dispatch-plan2.json`), the L14 plans (`l14-win-paused-plan.json`,
`l14-win-running-plan.json`), `parity_probe.py`,
`parity_diff.py`, `scripts/live867/` with `ck3agent/server/` beside it, and the Linux
recordings of the same build on the same save, taken on the node beforehand, in each
clock mode the Windows run uses (paused, and running at speed 1).
For step 4 that is `run.py --stability --clock running --save <node path of
p3probe_867_nodlc.ck3>` and the same with `--clock paused` (two runs each for the instability check; `--save` takes a name in
the save directory or a path; `--clock` has no default and a run without it exits 3).
Without `--save` the suite loads the node's `_867_01_01.ck3`, which lists
DLCs the VM does not own, so its replies are not a pair for the Windows run.

**The Windows bridge and the clock (cleanup 2026.11, L13 and L14).** No bridge
unpauses on its own, so a loaded save stays paused. Both bridges run their
game-thread work after every pass of the game's command thread, paused or running:
on Windows the bridge swaps the session thread's loop pointer (`session+0xB8`) for
a wrapper that calls the game's pass and then drains under the game's own execute
lock (an SRWLOCK, try-acquired as the game does), when the game's execute-commands
byte is 1 (`re/findings/windows/re_session_pass_hook_win.md` in the research
checkout). Under Proton (`docs/cleanup-2026.11/evidence/l14/win/`, run
`l14-win-live2`): the hook installs at game-ready, the bridge goes
`DORMANT -> ACTIVE` at tick 0 on the paused save, `query_character` and
`get_ai_personality` answer in about 50 ms while paused, a `set_ai_personality`
lands while paused and reads back from the game's own bytes, and
`get_tick_info.clock_writes` stays 0/0/0/0 until a session clock verb. The same
build without the hook (the phase-3 base, run `l14-win-basectl`) is the control:
see the evidence folder. This VM session re-checks that on real Windows (step 3a).

1. **Install** (`install.ps1`, Windows PowerShell 5.1 and again under PowerShell 7
   after `uninstall.ps1`). Pass: the identity check reports a match, `version.dll`
   sits beside `ck3.exe`, `.mcp.json` names `ck3agent.exe serve`. Then the refusal
   cases of `test_install_ps1.sh` by hand: a renamed other PE as `ck3.exe` is
   refused, a foreign `version.dll` is left alone.
2. **P4 again**: start CK3 from Steam to the menu. Pass: `%LOCALAPPDATA%\ck3agent\
   bridge.log` shows `identity match` and the forwards' call counts; the menu
   arrives in about the time it does without the DLL. Then switch the branch to the
   current public build: `identity REFUSED`, forwarding only, the game runs.
3. **P5 again**: load the reference save. It stays paused and the bridge goes
   ACTIVE without a tick.

   3a. **The session-pass hook on real Windows** (the L14 Proton result, re-checked).
   With the save loaded and never unpaused, in this order:
   - `bridge.log` has `Session-pass hook installed: slot 0x1455bc078 (session
     0x1455bbfc0 + 0xb8) 0x1432e63c0 -> session_pass wrapper ...; drain under SRWLOCK
     0x1455bc260 when execute_commands (+0x3f3) == 1` (the slot and the values are
     RVA-fixed: the image base is 0x140000000 unless the loader relocated it; a
     relocated base shifts all three by the same amount), then `SESSION-PASS: first
     drain on the game thread (tid=..., hook=session_pass, tick=0) — DORMANT -> ACTIVE`,
     and no `Session-pass hook: REFUSED`;
   - `parity_probe.py --endpoint %LOCALAPPDATA%\ck3agent\endpoint.json --mode control
     --plan l14-win-paused-plan.json --out <folder>\paused` (the research checkout's
     `re/scripts/windows/`). Pass: 10/10 answered; `get_tick_info` says `paused: true`,
     `tick: 0` and `clock_writes` all 0 at the first and the last step;
     `query_character 9870` answers with data (no `game_thread_timeout`) in well under
     a second; `set_ai_personality` (boldness 77, greed 13) answers success and the
     next `get_ai_personality` shows 77/13 in `engine`; the date is still 867.01.01;
   - then `--plan l14-win-running-plan.json --out <folder>\running`. Pass: after
     `unpause_game` the tick and the date move, reads answer while running,
     `pause_game` stops them, and `clock_writes.session` is 2 with the other three 0;
   - at exit (close the game normally once): `Session-pass hook removed: slot
     restored (in-flight 0)` in `bridge.log`, and no crash report.
   Any difference from the Proton record is a finding for the ADR's P7 row.

   3b. Then the parity plan: `parity_probe.py --endpoint
   %LOCALAPPDATA%\ck3agent\endpoint.json --plan plan-867.json --out <folder>\paused`,
   still paused. Pass: 526/526 answered, none hangs, no `game_thread_timeout`, the game
   does not crash. Then run the clock (`session(action="unpause")`, then
   `session(action="speed", speed=1)`) and run the plan again: `--out <folder>\running`.
   Pass: 526/526 answered; `parity_diff.py` against a Linux pair recorded the same way
   (the same save, two runs so the diff grades what Linux itself does not repeat)
   agrees with the Proton result (70 of 82 at full7 and full8, 2026-10-19) or better,
   every difference named (the open 6 and the one unwitnessed are listed in the P6
   findings). With the clock running, a step that reads something a game day changes
   can differ between the two Linux runs; the second run is what grades it.
4. **P6 again**, with the clock running at speed 1 (step 3): the steering plan
   (`--mode control`), the dispatch plan (`--mode control`;
   pass: gift to 13191 and to 39462 and sway to 13191 executed, befriend to 39462
   declined, the actor's gold 210.0 -> 11.25, as on Linux and under Proton), then the live suite attached to the
   game, with the reference save `p3probe_867_nodlc.ck3` loaded and the bridge ACTIVE:
   `python scripts/live867/run.py --endpoint %LOCALAPPDATA%\ck3agent\endpoint.json
   --clock running --out <folder>` (the Windows path: it reads `endpoint.json` the way
   the client does, and takes no lock, launches nothing and kills nothing; this step
   owns the game). `--clock` is required; run it once with `--clock running` and once
   with `--clock paused` (both work on Windows since the session-pass hook), each graded
   against a Linux pair taken in the same clock mode.
   `p3probe_867_nodlc.ck3` is `_867_01_01.ck3` with only its `dlcs={...}` list removed
   (`re/scripts/windows/strip_save_dlcs.py`), so its characters are those of the save
   `rulers.json` was taken from. The census scenario checks this on each side: it fails
   when a recorded ruler does not re-derive to the same id, and a census failure on
   either side stops the comparison.
   Pass: steering replies equal to Linux's apart from the date, the lock read back
   from the game's own bytes (`engine`), each scenario's status equal to Linux's and
   `scripts/live867/stability.py <linux results> <windows results>` reporting no
   `disagreed` scenario outside the fields the Linux pair already marks unstable.
5. **The product path**: `cd %USERPROFILE%\ck3agent-play`, `claude`, bind an AI
   ruler, one observe and one steer. Pass: the MCP server connects through
   `endpoint.json` with no configuration.
6. **Collect** `bridge.log`, the game's `logs\`, every recording, and the Windows
   event log entries for the game process, into one folder per step.

## When it is done

P7's condition holds when steps 1-5 pass, and the ADR's P7 row and the status
table say so with the run's date and the evidence folder. Anything that does not
reproduce the Proton result is a finding, named in the ADR, not a re-run until it
passes. The kill trigger ("Proton result does not reproduce on Windows") is judged
on that record.
