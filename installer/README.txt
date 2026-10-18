ck3agent: let Claude play Crusader Kings III (Linux x86_64, single-player).

  1. ./install.sh                  installs the bridge and the ck3agent command, runs setup
  2. ck3agent launch --continue    starts CK3 with the bridge and waits for the game
  3. cd ~/ck3agent-play && claude  Claude cannot play your own character: bind an AI ruler

VERSION holds the ck3agent release version. `ck3agent version` prints it with
the one CK3 build it supports; any other build is refused.
If something fails: `ck3agent doctor`.
Docs: https://github.com/skullbloc/ck3agent/blob/main/docs/getting-started.md
