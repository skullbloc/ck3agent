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
(`p6d-dispatch-plan2.json`), `parity_probe.py`,
`parity_diff.py`, `harness_diff.py`, the harness bundle, and the Linux recordings of
the same build (taken on the node beforehand, two runs for the instability check).

1. **Install** (`install.ps1`, Windows PowerShell 5.1 and again under PowerShell 7
   after `uninstall.ps1`). Pass: the identity check reports a match, `version.dll`
   sits beside `ck3.exe`, `.mcp.json` names `ck3agent.exe serve`. Then the refusal
   cases of `test_install_ps1.sh` by hand: a renamed other PE as `ck3.exe` is
   refused, a foreign `version.dll` is left alone.
2. **P4 again**: start CK3 from Steam to the menu. Pass: `%LOCALAPPDATA%\ck3agent\
   bridge.log` shows `identity match` and the forwards' call counts; the menu
   arrives in about the time it does without the DLL. Then switch the branch to the
   current public build: `identity REFUSED`, forwarding only, the game runs.
3. **P5 again**: load the reference save (paused, `CK3_SUPPRESS_AUTO_UNPAUSE=1` set
   in the user environment before starting Steam), run
   `parity_probe.py --endpoint %LOCALAPPDATA%\ck3agent\endpoint.json --plan plan-867.json`.
   Pass: 526/526 answered; `parity_diff.py` against the Linux pair agrees with the
   Proton result (70 of 82 at full7 and full8, 2026-10-19) or better, every difference
   named (the open 6 and the one unwitnessed are listed in the P6 findings).
4. **P6 again**: the steering plan (`--mode control`), the dispatch plan (`--mode control`;
   pass: gift to 13191 and to 39462 and sway to 13191 executed, befriend to 39462
   declined, the actor's gold 210.0 -> 11.25, as on Linux and under Proton), then the scenario harness
   (`python -m bridge.harness.run --endpoint ...`). Pass: steering replies equal to
   Linux's apart from the date, the lock read back from the game's own bytes
   (`engine`), the harness statuses equal to Linux's where Linux is stable.
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
