# Contributing

## 1. Where this repo comes from

This repository is exported from a private research tree; `UPSTREAM` names the
source commit. Your merged PR is carried upstream by the maintainer (`ported`),
adapted (`superseded`) or, rarely, dropped with a reason (`reverted`); the next
export commit names each PR in its trailers and credits you with
`Co-authored-by:`. The sync tooling identifies the release bot by name and
email, which anyone can type, so a PR containing a commit authored as the bot
is not merged. Merges here are merge commits, never squashes.

## 2. First contribution in 10 minutes

1. Pick an issue labelled `good first issue`, or one from [Good first issues](#good-first-issues).
2. Set up as in section 3. Change one thing, add a test beside it, run `make test`.
3. `git commit -s`, open a PR.

## 3. Dev setup

```bash
sudo apt-get install cmake build-essential git python3-venv python3-pip   # Debian, Ubuntu
sudo dnf install cmake gcc-c++ git python3-pip                            # Fedora
sudo pacman -S cmake base-devel git python-pip                            # Arch
python3 -m venv .venv && . .venv/bin/activate && pip install -e ".[test]"
```

Python 3.11 to 3.14. CMake downloads Zydis on first configure. `make help` lists targets. Details: [docs/building.md](docs/building.md).

## 4. Game-free tests

`make test` runs the server suite and `tests/` with pytest. No game needed.

* `pyproject.toml` deselects three upstream test ids that read research-tree files not in this repo.
* Tests sit beside the module: `ck3agent/server/test_<module>.py`. Basenames must be unique repo-wide; there is no `__init__.py`.
* `ck3agent/server/` uses bare sibling imports such as `from tools import ...`.
  Keep them; the files are shared verbatim with upstream. IDEs: pyright
  `extraPaths = ["ck3agent/server"]`, VS Code `python.analysis.extraPaths`.
* Native tests: `make bridge` builds the default target and runs `ctest` ([bridge/test/README.md](bridge/test/README.md)).

## 5. Changes that need the game

Hooks, offsets and anything touching game memory need a live check.
* Game: CK3 1.18.3.1, Linux native. See [docs/supported-versions.md](docs/supported-versions.md).
* Build exactly: `cmake -S bridge -B bridge/build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCK3_STRICT_BUILD_ID=ON`.
* Paste in the PR: `ck3agent --version`, `ck3agent doctor`, and the bridge log tail from the run.
* A loosened run, with `CK3_STRICT_BUILD_ID=0` or `--allow-unsupported-build` (alias `--unsupported`), is not evidence.

## 6. Offsets and RE

[re/README.md](re/README.md) describes the files and the tools and steps to re-derive them. Every entry in `re/offsets/ck3_offsets.h` carries
its evidence and a confidence: HIGH is 3+ independent sources or decompile-verified, MEDIUM is 2
sources or structural inference, LOW is a single unvalidated source.

Never overwrite a HIGH entry with a lower-confidence guess. If a value was
wrong, say why beside the fix. Offsets are base-relative, with the ASLR base
subtracted. Money, piety and prestige are int64 fixed-point, divided by 100000.
No game binaries, Ghidra projects or bulk decompiler output in PRs; quote the
lines that carry the evidence. Fetch a binary with your own account:

```bash
steamcmd +@sSteamCmdForcePlatformType windows +force_install_dir ./ck3-win \
         +login <you> +app_update 1158310 validate +quit   # or macos / linux
```

Addresses: Linux `ghidra_va = offset + 0x100000`. PE: `RVA = VA - ImageBase`.
Name the platform and BuildID, or the PE TimeDateStamp, in every finding.

## 7. Bridge code

* Handlers are one line each in `bridge/src/action_registry.inc`; the dispatch
  chain and the registry expand from it. A new action also needs a pause-state
  class in `bridge/src/snapshot.cpp`.
* Writes to game memory happen only on the AI tick. The IPC thread queues them.
* The SIGSEGV guards (`sigsetjmp`) are a crash net, not a safety boundary. Validate pointers before reading.
* Link nothing beyond libc, libm, libstdc++ and libgcc_s; CI checks `ldd`. Layout: [bridge/README.md](bridge/README.md).

## 8. Docs

`docs/` is CC-BY-4.0, everything else Apache-2.0 ([REUSE.toml](REUSE.toml)). The export generates
`docs/reference/tools.md` and `bridge-actions.md`; change the tool docstring or the action registry
instead. Keep prose short. Fix a wrong doc in the same PR as the code.

## 9. Port work

Windows and macOS are not supported yet. Open an issue first so the work can be split.
Design: [docs/adr/0001-windows-macos-port.md](docs/adr/0001-windows-macos-port.md). Labels:
`port:windows`, `port:macos`. Facts about a binary you own go in the `binary-report` issue form.

## 10. Style

Match the surrounding code; `.editorconfig` covers whitespace. One concern per PR. No
reformat-only or rename-only PRs: export overwrites files shared with upstream, and the churn is lost.

## 11. Glossary

Terms such as dormant, active, envelope and observer save are in [docs/glossary.md](docs/glossary.md).
Ids like `ck-xxxx` in comments are upstream tracker ids; see [bridge/test/README.md](bridge/test/README.md).

## 12. Review process

Add a line under the next version's heading in `CHANGELOG.md`
(`## YYYY.MM.N — date`; the release workflow reads that heading verbatim).
Skip it for docs-only and test-only changes.

CI must be green. CODEOWNERS routes the review; a maintainer merges here. Offset and bridge
changes need the live check in section 5 first. Merged PRs get `needs-port`, then `ported-upstream`.
First response within 7 days. No reply after 14: ping on the PR.

## 13. Credit and licensing

Sign off every commit with `git commit -s` to certify the [DCO](https://developercertificate.org/);
the sync does not port or credit a commit without a `Signed-off-by:` naming its author. Credit is
on by default; say so in the PR if you want none.
Copyright line: "Harrison Stebbins and ck3agent contributors".

## 14. Conduct and security

[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Vulnerabilities go in a private advisory per [SECURITY.md](SECURITY.md), never a public issue.

## Good first issues

1. Fold `ck3agent/_bridge_meta.py` into `ck3agent/_version.py`, so the package has one stamped version file; update `.github/workflows/release.yml` (its version check reads `_bridge_meta.py`) and `docs/building.md` in the same PR.
2. Fix the four `-Wmisleading-indentation` warnings in the bridge build. `cmake --build` prints them.
3. Remove the bridge's unused static functions; the build reports 19 `-Wunused-function` warnings.
