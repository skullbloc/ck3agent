# Supported versions

| ck3agent | CK3 build | Platform | BuildID |
|----------|-----------|----------|---------|
| 2026.10.0 | 1.18.3.1 (Crane) | Linux x86_64, Steam native | `e61ff2642f07094defd140fcc98e078d42541275` |

The bridge reads game memory through offsets derived by reverse engineering
one specific binary (`re/offsets/ck3_offsets.h`, ~7,400 lines with confidence
and evidence per entry). A different binary — another patch, the Windows
build under Proton, a beta branch — has different addresses, and reading
through the wrong ones ranges from silently wrong answers to a crash.

So the bridge **refuses to load** on any other BuildID: it logs

```
[CK3-Bridge] REFUSED: unsupported CK3 build.
[CK3-Bridge]   this bridge supports CK3 1.18.3.1 (linux), BuildID e61ff264…
[CK3-Bridge]   the running binary has BuildID …
```

opens no socket, hooks nothing, and the game runs unmodified. `ck3agent
setup` and `ck3agent doctor` make the same check from the ELF file before you
launch.

## When CK3 updates

Each game patch needs a new ck3agent release with re-derived offsets. Until
one exists:

* Keep the old binary. Steam's `Betas` tab often offers previous versions;
  pin CK3 to 1.18.3.1 there.
* `CK3_STRICT_BUILD_ID=0 ck3agent launch --unsupported` loosens the check.
  This is unsupported: some reads may work by luck, others will not, and a
  crash is possible. Do not report bugs from a loosened run.

## How to tell what you have

```
ck3agent doctor
```

reads the BuildID from the installed binary and compares it; the game's own
version string is in `launcher/launcher-settings.json` next to the binary.
