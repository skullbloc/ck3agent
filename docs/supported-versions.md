# Your game updated: what now?

Each ck3agent release supports exactly one CK3 build. After a Steam update,
the bridge refuses to load until a matching release exists. The game itself
still runs normally.

| ck3agent | CK3 build | BuildID |
|----------|-----------|---------|
| 2026.10.0 | 1.18.3.1, Linux native (Steam) | `e61ff2642f07094defd140fcc98e078d42541275` |

Why: the bridge reads game memory at addresses found by reverse engineering
one binary. On another binary those addresses are wrong, which gives wrong
answers or a crash.

## Prerequisites

- Steam with CK3 installed.

## Steps

1. **Back up your saves** before any version change. They are in
   `~/.local/share/Paradox Interactive/Crusader Kings III/save games/`.
2. **Roll back.** In Steam: right-click Crusader Kings III, Properties,
   Betas. Choose the branch named after 1.18.3.1 if one is offered.
3. **Stop further updates.** Properties, Updates, Automatic updates: "Only
   update this game when I launch it". `ck3agent launch` starts the binary
   directly, not through Steam's Play button.
4. **Watch for a matching release.** On GitHub: Watch, Custom, Releases on
   [skullbloc/ck3agent](https://github.com/skullbloc/ck3agent).

### Running anyway (unsupported)

```bash
ck3agent launch --continue --allow-unsupported-build
```

Some reads work, others return wrong values, and the game may crash or write
a corrupted save. Back up the save first. Do not report bugs from these runs.

## Check it worked

`ck3agent doctor` reports the CK3 build as supported.

## If it fails

- No 1.18.3.1 branch under Betas: wait for a new release, or run unsupported.
- The bridge log shows `REFUSED: unsupported CK3 build` and the BuildID it
  found. Include those lines in any issue about a new game version.
