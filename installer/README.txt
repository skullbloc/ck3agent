ck3agent — let Claude play Crusader Kings III (Linux x86_64)

  1. ./install.sh            installs the bridge + the ck3agent command, then runs setup
  2. ck3agent launch --continue
  3. cd ~/ck3agent-play && claude

Supported game build: see VERSION / `ck3agent version`. Any other CK3 build is
refused by the bridge with a message in /tmp/ck3_bridge.log.

Docs and source: https://github.com/skullbloc/ck3agent
Troubleshooting: `ck3agent doctor`
