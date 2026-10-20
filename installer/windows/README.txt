ck3agent: let Claude play Crusader Kings III (Windows x86_64, single-player). PREVIEW.

  1. Right-click install.ps1 > Run with PowerShell
     (or: powershell -ExecutionPolicy Bypass -File .\install.ps1)
     It installs the ck3agent command, checks your ck3.exe is the supported build,
     and places the bridge (version.dll) beside ck3.exe.
  2. Start Crusader Kings III from Steam and load a save.
  3. cd %USERPROFILE%\ck3agent-play and run claude. Claude cannot play your own
     character: bind an AI ruler.

Supported build: pin.json names the one ck3.exe this release works with (Steam:
Properties > Betas). On any other build the bridge stays off and the game runs
unmodified. Needs Python 3.11 or newer.

Remove: uninstall.ps1 (it only removes the version.dll it placed).
Windows support is a preview: see docs/adr/0001-windows-macos-port.md for its status.
