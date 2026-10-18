"""instance.py — THE naming authority for a CK3 instance's private paths.

Bead **ck-qn03** (R12), epic ck-6kvq. Design of record:
`docs/design/event-pipeline-conductor.md`; runbook `docs/multi-instance.md`.

WHY THIS FILE EXISTS
====================
Until this bead, one host could run exactly one episode. Four singletons made
that structural, and every one of them was a literal spelled out in a dozen
files:

  * the bridge IPC socket          ``/tmp/ck3-agent.sock``
  * the telemetry sink             ``/tmp/ck3-telemetry/``
  * the bridge's own log           ``/tmp/ck3_bridge.log``
  * the ops control directory      ``/tmp/ck3-shared/``

plus the headless display stack (one mutter, one wayland name, one state dir)
and CK3's own user-data directory, where ``continue_game.json``, ``last_save``
and the ``save games`` folder are per-USER, not per-process.

Two CK3 processes collide on all six. So an INSTANCE ID namespaces them, and
this module is the single place the naming is decided. There are three
transcriptions of these rules — this file, ``scripts/ck3_instance.sh`` (so the
launch chain needs no python) and ``bridge/src/instance_paths.cpp`` (so the
bridge, which is LD_PRELOADed into a process nobody wraps, resolves its own
paths) — and ``scripts/test_multi_instance.py`` pins all three against each
other. A fourth, private copy of the naming is the failure this design is
guarding against; do not add one.

THE DEFAULT INSTANCE IS THE LEGACY HOST
=======================================
``CK3_INSTANCE_ID`` unset, empty, or ``default`` resolves to the *historical*
paths, byte for byte. That is not a nicety: the live campaign, the self-heal
probe, the overlay poller, the watchdog and every runbook in ``docs/`` name
those paths, and an episode-isolation bead has no business moving them. A
non-default id moves everything under ``$CK3_INSTANCE_ROOT/<id>/``.

PRECEDENCE, in order
====================
  1. an explicit per-path override env var (``CK3_BRIDGE_SOCKET``, ``CK3_TELE``,
     ``CK3_BRIDGE_LOG``, ``CK3_CONTROL_DIR``) — always wins, so an operator can
     still point one client at one odd path;
  2. the instance id, if it is not the default;
  3. the legacy literal.

An id that does not match `ID_RE` is REFUSED (``InvalidInstanceId``) rather than
sanitised. A silently-rewritten id is how two "different" instances end up
sharing a socket, which is the exact bug this bead exists to remove.
"""

from __future__ import annotations

import os
import re
import shutil
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Mapping, Optional

# ── the naming constants ────────────────────────────────────────────────────

DEFAULT_INSTANCE_ID = "default"
DEFAULT_INSTANCE_ROOT = "/tmp/ck3-inst"

#: Legacy singletons — what the default instance resolves to, unchanged.
LEGACY_SOCKET = "/tmp/ck3-agent.sock"
LEGACY_TELEMETRY_DIR = "/tmp/ck3-telemetry"
LEGACY_BRIDGE_LOG = "/tmp/ck3_bridge.log"
LEGACY_CONTROL_DIR = "/tmp/ck3-shared"
LEGACY_HEADLESS_DIR = "/tmp/ck3-headless"
LEGACY_WAYLAND_NAME = "wayland-9"

#: An id must be a short filesystem- and socket-safe slug. The 48-char cap is
#: not cosmetic: ``sockaddr_un.sun_path`` is 108 bytes, and
#: ``/tmp/ck3-inst/<id>/agent.sock`` at 48 is 74 — comfortably inside it even
#: if ``CK3_INSTANCE_ROOT`` is moved somewhere longer. Episode ids
#: (``ep-20260912T033051Z-e0-paper-9eae1e0e``, 37 chars) fit as-is.
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$")

#: Env keys. `ENV_ID` and `ENV_ROOT` select; the rest override one path each.
ENV_ID = "CK3_INSTANCE_ID"
ENV_ROOT = "CK3_INSTANCE_ROOT"
ENV_SOCKET = "CK3_BRIDGE_SOCKET"
ENV_TELEMETRY = "CK3_TELE"
ENV_BRIDGE_LOG = "CK3_BRIDGE_LOG"
ENV_CONTROL_DIR = "CK3_CONTROL_DIR"


class InvalidInstanceId(ValueError):
    """An instance id that is not a `ID_RE` slug. Never sanitised — see module doc."""


def normalise_id(raw: Optional[str]) -> str:
    """`None`/empty -> `default`; otherwise validate and return unchanged."""
    if raw is None:
        return DEFAULT_INSTANCE_ID
    raw = raw.strip()
    if not raw:
        return DEFAULT_INSTANCE_ID
    if not ID_RE.match(raw):
        raise InvalidInstanceId(
            f"instance id {raw!r} is not a valid slug "
            f"(want {ID_RE.pattern}); refusing to sanitise it")
    return raw


def is_default(instance_id: Optional[str]) -> bool:
    return normalise_id(instance_id) == DEFAULT_INSTANCE_ID


@dataclass(frozen=True)
class InstancePaths:
    """Every per-instance path, resolved. Construct via `resolve()`."""

    instance_id: str
    root: str
    instance_dir: str
    socket: str
    telemetry_dir: str
    bridge_log: str
    control_dir: str
    headless_dir: str
    wayland_name: str
    #: ``XDG_DATA_HOME`` for the game process, or None to inherit the user's.
    #:
    #: ⚠ ck-wfz1: THIS DOES NOT WORK FOR THE GAME'S USER-DATA DIRECTORY, and the
    #: comment that used to stand here was an inference from a string rather than
    #: a measurement. It said: "CK3 reads $XDG_DATA_HOME before $HOME/.local/share
    #: (the binary carries the string 'neither XDG_DATA_HOME nor HOME environment
    #: is set'), which is how two games get private continue_game.json / save
    #: games trees without a command-line switch the game does not have."
    #:
    #: THE STRING IS REALLY IN THE BINARY. So is
    #: ``/.local/share/Paradox Interactive/launcher/session`` one line away, which
    #: is the likelier consumer. MEASURED on a live game (ck-9rto, 2026-09-17):
    #: with ``XDG_DATA_HOME`` set to the instance tree in ``/proc/<pid>/environ``,
    #: the process held FIFTEEN open write descriptors on
    #: ``$HOME/.local/share/Paradox Interactive/Crusader Kings III/logs/`` and the
    #: instance's own ``save games`` directory was never written or read. A STRING
    #: IN A BINARY IS NOT A CODE PATH THAT RUNS (ck-nkyc's caller-less
    #: ``run_resolver``, ck-fvms.1's ``DISPATCH_TIMEOUT_SECS``, and now this).
    #:
    #: The variable is still exported — it costs nothing and something does read
    #: it — but the SIXTH SINGLETON in docs/multi-instance.md's table is NOT
    #: namespaced, and two concurrent games would share one user-data tree.
    #: ``scripts/instance_write_witness.py`` is the check; it FAILS today.
    xdg_data_home: Optional[str]
    #: CK3's own user-data directory *inside* `xdg_data_home` (None when the
    #: instance inherits the user's).
    game_user_dir: Optional[str]

    # -- convenience ---------------------------------------------------------

    @property
    def is_default(self) -> bool:
        return self.instance_id == DEFAULT_INSTANCE_ID

    @property
    def action_log(self) -> str:
        return str(Path(self.telemetry_dir) / "action_log.jsonl")

    @property
    def cost_hourly(self) -> str:
        return str(Path(self.telemetry_dir) / "cost-hourly.json")

    @property
    def overlay_state(self) -> str:
        return str(Path(self.telemetry_dir) / "overlay-state.json")

    def as_dict(self) -> dict:
        d = asdict(self)
        d["action_log"] = self.action_log
        return d

    def env(self) -> dict:
        """The env a child process needs to *be* this instance.

        Every key is set explicitly, including on the default instance, so a
        child inherits an unambiguous instance rather than re-deriving one from
        a variable a wrapper might have dropped. `XDG_DATA_HOME` appears only
        for a non-default instance — exporting it on the default would move the
        live campaign's save games, which is precisely the harm this bead is
        supposed to prevent.
        """
        e = {
            ENV_ID: self.instance_id,
            ENV_ROOT: self.root,
            ENV_SOCKET: self.socket,
            ENV_TELEMETRY: self.telemetry_dir,
            ENV_BRIDGE_LOG: self.bridge_log,
            ENV_CONTROL_DIR: self.control_dir,
        }
        if self.xdg_data_home:
            e["XDG_DATA_HOME"] = self.xdg_data_home
        return e

    def mkdirs(self) -> "InstancePaths":
        """Create every directory this instance writes into. Idempotent."""
        for d in (self.instance_dir, self.telemetry_dir, self.control_dir):
            Path(d).mkdir(parents=True, exist_ok=True)
        Path(self.socket).parent.mkdir(parents=True, exist_ok=True)
        Path(self.bridge_log).parent.mkdir(parents=True, exist_ok=True)
        # 0777 on the control dir preserves the legacy two-user contract
        # (CLAUDE.md "Shared directory"): `admin` writes commands a daemon
        # running as another user reads. Applied only to a NAMED instance's own
        # directory — /tmp/ck3-shared already exists on the campaign host with
        # whatever mode the operator gave it, and widening that silently is not
        # this bead's business.
        if not self.is_default:
            try:
                os.chmod(self.control_dir, 0o777)
            except OSError:
                pass
        return self


def resolve(instance_id: Optional[str] = None,
            env: Optional[Mapping[str, str]] = None) -> InstancePaths:
    """Resolve every path for `instance_id` (default: `$CK3_INSTANCE_ID`).

    Pure: reads `env` (default `os.environ`), touches no filesystem.
    """
    env = os.environ if env is None else env
    iid = normalise_id(instance_id if instance_id is not None else env.get(ENV_ID))
    root = env.get(ENV_ROOT) or DEFAULT_INSTANCE_ROOT
    inst_dir = str(Path(root) / iid)
    default = iid == DEFAULT_INSTANCE_ID

    def pick(override_key: str, legacy: str, member: str) -> str:
        ov = env.get(override_key)
        if ov:
            return ov
        return legacy if default else str(Path(inst_dir) / member)

    xdg = None if default else str(Path(inst_dir) / "xdg")
    return InstancePaths(
        instance_id=iid,
        root=root,
        instance_dir=inst_dir,
        socket=pick(ENV_SOCKET, LEGACY_SOCKET, "agent.sock"),
        telemetry_dir=pick(ENV_TELEMETRY, LEGACY_TELEMETRY_DIR, "telemetry"),
        bridge_log=pick(ENV_BRIDGE_LOG, LEGACY_BRIDGE_LOG, "bridge.log"),
        control_dir=pick(ENV_CONTROL_DIR, LEGACY_CONTROL_DIR, "shared"),
        headless_dir=LEGACY_HEADLESS_DIR if default else str(Path(inst_dir) / "headless"),
        wayland_name=LEGACY_WAYLAND_NAME if default else f"wayland-ck3-{iid}",
        xdg_data_home=xdg,
        game_user_dir=(None if default
                       else str(Path(xdg) / "Paradox Interactive" / "Crusader Kings III")),
    )


# ── the shorthands every client actually calls ──────────────────────────────

def socket_path(instance_id: Optional[str] = None) -> str:
    return resolve(instance_id).socket


def telemetry_dir(instance_id: Optional[str] = None) -> str:
    return resolve(instance_id).telemetry_dir


def telemetry_file(name: str, instance_id: Optional[str] = None) -> str:
    return str(Path(resolve(instance_id).telemetry_dir) / name)


def bridge_log(instance_id: Optional[str] = None) -> str:
    return resolve(instance_id).bridge_log


def control_dir(instance_id: Optional[str] = None) -> str:
    return resolve(instance_id).control_dir


def current_id(env: Optional[Mapping[str, str]] = None) -> str:
    return normalise_id((os.environ if env is None else env).get(ENV_ID))


# ── the game's private user-data tree ───────────────────────────────────────

#: Entries of the real CK3 user directory that a private instance must OWN
#: rather than share. Everything else is symlinked to the user's real tree, so
#: an instance inherits settings, dlc_load, mods and the shader cache without
#: copying ~100 MB of them.
PRIVATE_USER_DIR_ENTRIES = (
    "save games",        # the episode's own saves; autosave names collide
    "logs",              # error.log / system.log, opened for write at start
    "crashes",
    "dumps",
    "exceptions",
    "cache",
    "shadercache",
    "player",
    "pops_filestorage",
)

#: Files (not directories) that must be private and START ABSENT. `last_save`
#: is here because CK3 WRITES it: symlinking a 105 MB save the game overwrites
#: would have every instance stamping the user's real tree.
PRIVATE_USER_DIR_FILES = (
    "continue_game.json",
    "pops_filestorage.index",
    "last_save.ck3",
)


def prepare_game_user_dir(paths: InstancePaths,
                          real_user_dir: Optional[str] = None) -> Optional[str]:
    """Build the instance's private CK3 user directory. Returns its path.

    A no-op returning None for the default instance — it uses the user's real
    directory, on purpose.

    THE RULE, and it is about WRITES, not about size:

      * the directories CK3 writes into (`PRIVATE_USER_DIR_ENTRIES`) and the
        files it rewrites (`PRIVATE_USER_DIR_FILES`) are PRIVATE and start
        empty or absent;
      * every other FILE is COPIED, never symlinked. `pdx_settings.txt`,
        `dlc_load.json` and the launcher sqlite are all small and all written
        back by the game or the launcher, and a symlink would carry that write
        into the user's real tree — one instance changing a setting for
        everybody, invisibly;
      * every other DIRECTORY is SYMLINKED (`mod/`, `.launcher-cache/`), because
        those are installed content the instances must agree about and are far
        too big to copy per instance.
    """
    if paths.xdg_data_home is None:
        return None
    real = Path(real_user_dir).expanduser() if real_user_dir else (
        Path.home() / ".local" / "share" / "Paradox Interactive" / "Crusader Kings III")
    dst = Path(paths.game_user_dir)
    dst.mkdir(parents=True, exist_ok=True)

    # Steam lives under XDG_DATA_HOME too; a direct launch still resolves the
    # runtime and the game's install through it, so link it across.
    xdg = Path(paths.xdg_data_home)
    real_steam = Path.home() / ".local" / "share" / "Steam"
    link = xdg / "Steam"
    if real_steam.exists() and not link.exists():
        link.symlink_to(real_steam)

    # Sibling launcher state, read-only for our purposes.
    for sib in ("launcherpath", "launcher-v2"):
        s = real.parent / sib
        d = dst.parent / sib
        if s.exists() and not d.exists():
            d.symlink_to(s)

    private = set(PRIVATE_USER_DIR_ENTRIES) | set(PRIVATE_USER_DIR_FILES)
    if real.is_dir():
        for entry in real.iterdir():
            if entry.name in private:
                continue
            target = dst / entry.name
            if target.exists() or target.is_symlink():
                continue
            if entry.is_dir():
                target.symlink_to(entry)
            else:
                try:
                    shutil.copy2(entry, target)
                except OSError:
                    # Unreadable or vanished: better to be missing than to be a
                    # symlink the game can write through.
                    pass
    for name in PRIVATE_USER_DIR_ENTRIES:
        (dst / name).mkdir(parents=True, exist_ok=True)
    return str(dst)


# ── CLI: the shell helper's fallback and a human-readable dump ──────────────

def _main(argv=None) -> int:
    import argparse
    import json
    import shlex

    ap = argparse.ArgumentParser(description="Resolve a CK3 instance's paths.")
    ap.add_argument("command", choices=("env", "json", "paths", "mkdirs", "gamedir"))
    ap.add_argument("-i", "--instance-id", default=None)
    ap.add_argument("--export", action="store_true",
                    help="env: prefix each line with `export` for eval")
    a = ap.parse_args(argv)

    p = resolve(a.instance_id)
    if a.command == "mkdirs":
        p.mkdirs()
        print(p.instance_dir)
    elif a.command == "gamedir":
        p.mkdirs()
        print(prepare_game_user_dir(p) or "")
    elif a.command == "json":
        print(json.dumps(p.as_dict(), indent=2, sort_keys=True))
    elif a.command == "paths":
        for k, v in sorted(p.as_dict().items()):
            print(f"{k:16s} {v}")
    else:
        pre = "export " if a.export else ""
        for k, v in p.env().items():
            print(f"{pre}{k}={shlex.quote(v)}")
    return 0


if __name__ == "__main__":      # pragma: no cover - CLI
    raise SystemExit(_main())
