ck3agent: let Claude play Crusader Kings III (Windows x86_64, single-player). PREVIEW.

  1. Right-click install.ps1 > Run with PowerShell
     (or: powershell -ExecutionPolicy Bypass -File .\install.ps1)
     It installs the ck3agent command, checks your ck3.exe is the supported build,
     places the bridge (version.dll) beside ck3.exe, and writes the play directory
     (.mcp.json, the play protocol CLAUDE.md, .claude\settings.json).
  2. Start Crusader Kings III from Steam and load a save.
  3. cd %USERPROFILE%\ck3agent-play and run claude. Claude cannot play your own
     character: bind an AI ruler.

The tools: the ck3 server serves 13 (observe, inspect, query, discover, cbs,
plan_war, declare_war, do, steer, succession_plan, set_phase, pass_turn,
session); the play protocol, CLAUDE.md in the play directory, says how each is
called. On Windows some writes answer a typed refusal (reason not_on_windows,
naming the game function this build has not mapped yet) instead of acting.

Supported build: pin.json names the one ck3.exe this release works with (Steam:
Properties > Betas). On any other build the bridge stays off and the game runs
unmodified. Needs Python 3.11 or newer.

Settings (environment variables, all optional):
  CK3AGENT_GAME_DIR   the Crusader Kings III directory (else found through Steam)
  CK3AGENT_DATA_DIR   where the venv and install.json live (else %LOCALAPPDATA%\ck3agent)
  CK3AGENT_PLAY_DIR   the play directory (else %USERPROFILE%\ck3agent-play)
  CK3_BRIDGE_PORT     the bridge's TCP loopback port (else a free one)
The bridge writes bridge.log and endpoint.json (its port and token, which the
server reads) to %LOCALAPPDATA%\ck3agent, whatever CK3AGENT_DATA_DIR says;
with CK3_INSTANCE_ID set, to %LOCALAPPDATA%\ck3agent\inst\<id>.

Remove: uninstall.ps1 (it only removes the version.dll it placed).
Windows support is a preview: see docs/adr/0001-windows-macos-port.md for its status.
