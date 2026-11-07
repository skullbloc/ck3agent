"""ck3agent command line. `ck3agent --help` lists the commands."""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import logging
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import tomllib
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from . import __version__, bootstrap_server, server_dir
from . import _bridge as br
from . import _config as cfg
from . import _console as con
from ._bridge_meta import SUPPORTED_BUILD_ID, SUPPORTED_GAME_VERSION, SUPPORTED_PLATFORM
from ._elf import NotAnElf, read_build_id

PROTOCOL_DIR = Path(__file__).resolve().parent / "protocol"
MCP_SERVER_NAME = "ck3"
ALLOW = [f"mcp__{MCP_SERVER_NAME}"]
# What an earlier setup wrote into permissions.allow and this one takes back out.
# The protocol never sleeps in Bash (time passes through session(action="wait")),
# so a sleep allowance only let a run do what the protocol forbids.
RETIRED_ALLOW = ["Bash(sleep:*)"]
# Claude Code hands an MCP call still running after 120 s to a background task;
# session(action="wait") blocks for up to 110 s by design, and a long read can
# push past it. Off in the play directory and in `ck3agent run`.
SETTINGS_ENV = {"CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS": "0"}
# Claude Code defers MCP tool schemas behind ToolSearch unless the server entry
# says `alwaysLoad: true` ("all tools from this server are always included in
# the prompt and never deferred behind tool search"; in the stdio server schema
# of Claude Code 2.1.241, the pilots' version, and 2.1.318). Without it every
# pilot seat spent 1-7 turns loading the 13 schemas (90 ToolSearch calls in 34
# seats, pilots 2026.11). Setup writes it into a new ck3 entry; `run` sets it on
# the run's own copy of the entry, so an older play dir gets it too.
MCP_ALWAYS_LOAD = {"alwaysLoad": True}
RELEASES_URL = "https://github.com/skullbloc/ck3agent/blob/main/docs/supported-versions.md"
CK3_EXAMPLE = '"$HOME/.local/share/Steam/steamapps/common/Crusader Kings III/binaries/ck3"'
CONTINUE_WAIT = 120
#: The default wait when the game directory's shader cache is empty: CK3 compiles
#: it on its first start there (about 590 MB; the 2026.12.1 node smoke's first
#: load ran past 120 s while still compiling).
FIRST_LOAD_WAIT = 600
LOOPBACK = {"127.0.0.1", "localhost", "::1"}
VERSION_LINE = (f"ck3agent {__version__} (CK3 {SUPPORTED_GAME_VERSION} {SUPPORTED_PLATFORM}, "
                f"BuildID {SUPPORTED_BUILD_ID})")
EXIT_TIMEOUT = 3


# ── shared helpers ───────────────────────────────────────────────────────────

def _runnable(p: Path) -> bool:
    return p.is_file() and os.access(p, os.X_OK)


def exe_argv() -> list[str]:
    """How Claude Code starts this install. One answer for .mcp.json and the hook.

    install.sh builds each venv at DATA/venv.<version>.<pid> and points the
    stable DATA/venv at it, deleting older ones. The path written out goes
    through DATA/venv, so it outlives the next reinstall.
    """
    sib = Path(sys.executable).parent / "ck3agent"
    stable = cfg.data_dir() / "venv"
    with contextlib.suppress(OSError, RuntimeError):
        if stable.exists() and stable.resolve() == Path(sys.prefix).resolve() \
                and _runnable(stable / "bin" / "ck3agent"):
            return [str(stable / "bin" / "ck3agent")]
    if _runnable(sib):
        return [str(sib)]
    return [sys.executable, "-m", "ck3agent"]


def command_exists(cmd) -> bool:
    """Whether an .mcp.json `command` names something that can run."""
    if not isinstance(cmd, str) or not cmd:
        return False
    if os.sep in cmd:
        return _runnable(Path(cmd).expanduser())
    return shutil.which(cmd) is not None


def preload_ok(path: Path) -> bool:
    """LD_PRELOAD splits on ':' and whitespace, so the path may contain neither."""
    s = str(path)
    return ":" not in s and not any(ch.isspace() for ch in s)


def unsupported_message(ver: str, bid: str) -> str:
    return (f"Your game: CK3 {ver}, BuildID {bid[:12]}. ck3agent {__version__} needs "
            f"CK3 {SUPPORTED_GAME_VERSION}, BuildID {SUPPORTED_BUILD_ID[:12]}.\n"
            f"See {RELEASES_URL}")


def check_build(binary: Path) -> tuple[bool, str]:
    """Print the BuildID verdict. Returns (supported, message-if-not)."""
    try:
        bid = read_build_id(binary)
    except (NotAnElf, OSError) as e:
        con.fail("cannot read BuildID", str(e))
        return False, f"{binary} is not a readable CK3 binary. Pass --ck3 PATH."
    ver = cfg.game_version_from_install(binary) or "unknown version"
    if bid == SUPPORTED_BUILD_ID:
        con.ok("CK3 build supported", f"{ver}, BuildID {bid[:12]}")
        return True, ""
    con.fail("CK3 build not supported", f"{ver}, BuildID {bid or 'none'}")
    return False, unsupported_message(ver, bid or "none")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _now_utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _state_file() -> Path:
    return cfg.data_dir() / "state" / "last-launch.json"


def _logs_dir() -> Path:
    return cfg.data_dir() / "logs"


def _load_json(path: Path) -> dict:
    """{} for a missing file; ValueError for one that is not a JSON object."""
    if not path.exists():
        return {}
    d = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(d, dict):
        raise ValueError(f"{path}: not a JSON object")
    return d


def _write_json(path: Path, d: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(d, indent=2) + "\n", encoding="utf-8")


# ── setup ────────────────────────────────────────────────────────────────────

def merge_mcp(d: dict, argv: list[str], force: bool) -> bool:
    """Add the ck3 server to an .mcp.json dict in place. True if it changed.

    An existing ck3 entry is kept unless `force`, or unless its command no
    longer exists (a reinstall or a pipx<->venv switch moved it).
    """
    servers = d.setdefault("mcpServers", {})
    if not isinstance(servers, dict):
        raise ValueError("mcpServers is not an object")
    entry = {"command": argv[0], "args": argv[1:] + ["serve"], **MCP_ALWAYS_LOAD}
    cur = servers.get(MCP_SERVER_NAME)
    stale = isinstance(cur, dict) and not command_exists(cur.get("command"))
    if cur == entry or (MCP_SERVER_NAME in servers and not force and not stale):
        return False
    servers[MCP_SERVER_NAME] = entry
    return True


def _is_our_hook(h) -> bool:
    cmd = h.get("command", "") if isinstance(h, dict) else ""
    return isinstance(cmd, str) and "ck3agent" in cmd and cmd.rstrip().endswith("hook-state")


def merge_settings(d: dict, hook_cmd: str | None, allow: list[str] | None,
                   env: dict[str, str] | None = None) -> dict:
    """Add our hook, allow-list and env to a .claude/settings.json dict, keeping the rest."""
    if env:
        block = d.setdefault("env", {})
        if not isinstance(block, dict):
            raise ValueError("env is not an object")
        block.update(env)
    if hook_cmd:
        groups = d.setdefault("hooks", {}).setdefault("UserPromptSubmit", [])
        if not isinstance(groups, list):
            raise ValueError("hooks.UserPromptSubmit is not a list")
        kept = []
        for g in groups:
            if isinstance(g, dict) and isinstance(g.get("hooks"), list) and any(map(_is_our_hook, g["hooks"])):
                g = {**g, "hooks": [h for h in g["hooks"] if not _is_our_hook(h)]}
                if not g["hooks"]:
                    continue
            kept.append(g)
        kept.append({"hooks": [{"type": "command", "command": hook_cmd}]})
        groups[:] = kept
    perms = d.get("permissions")
    if allow or isinstance(perms, dict) and "allow" in perms:
        # A retired rule an earlier setup wrote goes, with or without --no-allow.
        rules = d.setdefault("permissions", {}).setdefault("allow", [])
        if not isinstance(rules, list):
            raise ValueError("permissions.allow is not a list")
        rules[:] = [r for r in rules if r not in RETIRED_ALLOW]
        rules += [r for r in allow or [] if r not in rules]
    return d


def cmd_setup(a: argparse.Namespace) -> int:
    c = cfg.Config.load()
    print(f"ck3agent {__version__} setup (needs CK3 {SUPPORTED_GAME_VERSION}, {SUPPORTED_PLATFORM})")

    binary = cfg.resolve("ck3_binary", c, a.ck3).path
    if not binary or not binary.is_file():
        con.fail("CK3 binary not found", str(binary) if binary else "not in any Steam library")
        con.note(f"pass --ck3 {CK3_EXAMPLE}")
        con.note("Steam > Crusader Kings III > Manage > Browse local files opens that folder.")
        return 1
    con.ok("CK3 binary", str(binary))
    supported, why = check_build(binary)

    bridge = cfg.resolve("bridge_so", c, a.bridge).path
    bridge_ok = False
    if not bridge or not bridge.is_file():
        con.fail("bridge library not found",
                 f"expected {cfg.data_dir() / 'lib/libck3_bridge.so'}; run install.sh or pass --bridge")
    elif not preload_ok(bridge.resolve()):
        con.fail("bridge path unusable", f"{bridge.resolve()}: LD_PRELOAD cannot hold ':' or spaces; move it")
    else:
        bridge, bridge_ok = bridge.resolve(), True
        con.ok("bridge library", str(bridge))

    if not supported:
        print(f"\n{why}\nNothing written. Rerun setup once the game matches.")
    if not (supported and bridge_ok):
        return 1

    src_md = PROTOCOL_DIR / "CLAUDE.md"
    if not src_md.is_file():
        con.fail("package incomplete", f"{src_md} missing; reinstall ck3agent")
        return 1

    play = cfg.resolve("play_dir", c, a.play_dir).path.resolve()
    play.mkdir(parents=True, exist_ok=True)
    argv = exe_argv()
    mcp_path = play / ".mcp.json"
    settings_path = play / ".claude" / "settings.json"
    try:
        mcp = _load_json(mcp_path)
        settings = _load_json(settings_path)
        changed = merge_mcp(mcp, argv, a.force)
        hook_cmd = None if a.no_hook else " ".join(shlex.quote(x) for x in argv + ["hook-state"])
        merge_settings(settings, hook_cmd, None if a.no_allow else ALLOW, SETTINGS_ENV)
    except ValueError as e:
        con.fail("cannot merge existing file", f"{e}; fix it or remove it")
        return 1

    if changed:
        _write_json(mcp_path, mcp)
        con.ok(".mcp.json written", str(mcp_path))
    else:
        con.ok(".mcp.json kept", f"{mcp_path} (--force replaces the ck3 entry)")
    md = play / "CLAUDE.md"
    if md.exists() and not a.force:
        con.ok("CLAUDE.md kept", str(md))
    else:
        shutil.copy(src_md, md)
        con.ok("CLAUDE.md written", "the play protocol")
    _write_json(settings_path, settings)
    what = [w for w, on in (("live-state hook", hook_cmd), ("allow-list", not a.no_allow),
                            ("env (no MCP auto-background: session wait blocks up to 110 s)", True)) if on]
    con.ok(".claude/settings.json merged", ", ".join(what))

    c.ck3_binary, c.bridge_so = str(binary), str(bridge)
    c.play_dir = str(play)
    con.ok("config saved", str(c.save()))
    print()
    print("Steam launch options, if you start CK3 from Steam:")
    print(f"  LD_PRELOAD={bridge} %command%")
    print("Next:")
    print("  ck3agent launch --continue")
    print(f"  cd {shlex.quote(str(play))} && claude")
    if not a.no_allow and not trust_record(play)[0]:
        print("  The first time, accept Claude Code's trust dialog for this directory: until then")
        print("  it ignores the allow-list setup wrote. (`ck3agent run` passes it on the command line.)")
    return 0


# ── doctor ───────────────────────────────────────────────────────────────────

class _Checks:
    def __init__(self) -> None:
        self.failed = 0
        self.next_fail: str | None = None
        self.next_warn: str | None = None

    def fail(self, label: str, detail: str, nxt: str) -> None:
        con.fail(label, detail)
        self.failed += 1
        self.next_fail = self.next_fail or nxt

    def warn(self, label: str, detail: str, nxt: str | None = None) -> None:
        con.warn(label, detail)
        self.next_warn = self.next_warn or nxt


def _check_mcp_entry(k: _Checks, path: Path) -> None:
    """The command Claude Code will run for the ck3 server must exist."""
    try:
        entry = (_load_json(path).get("mcpServers") or {}).get(MCP_SERVER_NAME)
    except (ValueError, OSError, AttributeError) as e:
        k.fail(".mcp.json unreadable", str(e), f"fix or remove {path}, then ck3agent setup")
        return
    if not isinstance(entry, dict):
        k.fail(".mcp.json has no ck3 server", str(path), "ck3agent setup")
    elif not command_exists(entry.get("command")):
        k.fail("ck3 MCP command missing", f"{entry.get('command')!r} in {path} does not exist "
               "(moved by a reinstall?)", "ck3agent setup (it replaces a missing command)")
    else:
        con.ok("ck3 MCP command", str(entry.get("command")))


#: What Claude Code prints, on every `claude -p` in a play directory whose
#: workspace was never trusted (smoke17, each run's stderr.log):
#: "Ignoring 1 permissions.allow entry from .claude/settings.json: this workspace
#: has not been trusted. Run Claude Code interactively here once and accept the
#: trust dialog, or set projects["<dir>"].hasTrustDialogAccepted: true in ~/.claude.json."
TRUST_KEY = "hasTrustDialogAccepted"


def trust_record(play: Path, env=None) -> tuple[bool | None, str]:
    """(trusted?, where) from Claude Code's own record, read only.

    None when it cannot be read: CLAUDE_CONFIG_DIR moves Claude Code's state and
    this does not guess where, and an unreadable file is not "untrusted"."""
    env = os.environ if env is None else env
    if env.get("CLAUDE_CONFIG_DIR"):
        return None, "CLAUDE_CONFIG_DIR is set"
    path = Path.home() / ".claude.json"
    if not path.exists():
        return False, str(path)
    try:
        projects = _load_json(path).get("projects")
    except (ValueError, OSError) as e:
        return None, f"{path}: {e}"
    entry = projects.get(str(play.resolve())) if isinstance(projects, dict) else None
    return (isinstance(entry, dict) and entry.get(TRUST_KEY) is True), str(path)


def _check_trust(k: _Checks, play: Path) -> None:
    trusted, where = trust_record(play)
    if trusted:
        con.ok("Claude Code trusts the play directory", where)
    elif trusted is None:
        con.warn("Claude Code trust: not read", where)
    else:
        # A note, not the next step: the game comes first, and `run` is unaffected.
        con.warn("Claude Code has not trusted the play directory",
                 f"no {TRUST_KEY} for {play.resolve()} in {where}: until it does, the allow-list "
                 "in .claude/settings.json is ignored (`ck3agent run` passes it on the command line)")
        con.note(f"the first `cd {shlex.quote(str(play))} && claude` asks; accept the trust dialog once")


def cmd_doctor(a: argparse.Namespace) -> int:
    k = _Checks()
    print(f"ck3agent {__version__} doctor (needs CK3 {SUPPORTED_GAME_VERSION}, {SUPPORTED_PLATFORM})")
    py = sys.version.split()[0]
    if sys.version_info < (3, 11):
        k.fail("python", f"{py}, need 3.11+", "install Python 3.11 or newer")
    else:
        con.ok("python", py)
    try:
        import mcp  # noqa: F401
        con.ok("mcp package", "present")
    except ImportError:
        k.fail("mcp package missing", "", "pip install ck3agent")

    c = cfg.Config.load()
    if cfg.config_path().exists():
        con.ok("config", str(cfg.config_path()))
    else:
        k.warn("no config yet", str(cfg.config_path()), "ck3agent setup")

    binary = cfg.resolve("ck3_binary", c, a.ck3).path
    if binary and binary.is_file():
        con.ok("CK3 binary", str(binary))
        supported, why = check_build(binary)
        if not supported:
            k.failed += 1
            k.next_fail = k.next_fail or why
    else:
        k.fail("CK3 binary not found", str(binary or "not in any Steam library"),
               f"ck3agent setup --ck3 {CK3_EXAMPLE}")

    bridge = cfg.resolve("bridge_so", c, a.bridge).path
    if not bridge or not bridge.is_file():
        k.fail("bridge library not found", str(bridge or cfg.data_dir() / "lib/libck3_bridge.so"),
               "rerun install.sh, or ck3agent setup --bridge PATH")
    elif not preload_ok(bridge):
        k.fail("bridge path unusable", f"{bridge}: no ':' or spaces allowed", "move the .so and rerun setup")
    else:
        con.ok("bridge library", str(bridge))

    claude = shutil.which("claude")
    if claude:
        con.ok("claude CLI", claude)
    else:
        k.warn("claude CLI not on PATH", "needed for Claude Code play and `ck3agent run`")

    # The play directory doctor checks is the one it is told: --play-dir, or the
    # one setup recorded in the config. With neither, a ~/ck3agent-play left by
    # another install (or another XDG config) is not this install's and is not read.
    rp = cfg.resolve("play_dir", c, a.play_dir)
    play = rp.path
    if rp.source == "default":
        k.warn("play directory not set up", f"no --play-dir and none recorded in {cfg.config_path()}",
               "ck3agent setup")
    elif (play / ".mcp.json").exists():
        con.ok("play directory", f"{play} ({rp.source})")
        _check_mcp_entry(k, play / ".mcp.json")
        _check_trust(k, play)
    else:
        k.warn("play directory not set up", f"{play} has no .mcp.json", "ck3agent setup")

    sock = br.socket_path()
    game = br.alive(sock)
    state = ""
    if game:
        con.ok("bridge socket", sock)
        ok, state = br.handshake_check(sock)
        if not ok:
            k.failed += 1
            k.next_fail = k.next_fail or "restart the game: ck3agent launch --continue"
    else:
        log = Path(br.bridge_log_path())
        hit = br.find_refusal(log, br.last_launch_time(_logs_dir()))
        if hit:
            mtime = datetime.fromtimestamp(log.stat().st_mtime).strftime("%Y-%m-%d %H:%M")
            k.fail("bridge refused to load", f"{log} (written {mtime})",
                   f"your game build is not supported; see {RELEASES_URL}")
            for line in hit:
                con.note(line)
        else:
            con.warn("bridge socket not listening", f"{sock}: CK3 is not running with the bridge")

    print()
    if k.failed:
        print(f"{k.failed} check(s) failed. Next: {k.next_fail}")
        return 1
    print(doctor_last_line(k.next_warn, game, state, play))
    return 0


def doctor_last_line(next_warn: str | None, game: bool, state: str, play: Path) -> str:
    """The verdict line. "Ready" only when the game is active: a bridge that
    answers from the menu (`dormant`) is not a game Claude can play yet."""
    if next_warn:
        return f"Next: {next_warn}"
    if not game:
        return "Ready. Next: ck3agent launch --continue"
    if state != "active":
        return (f"Game not active yet (session_state={state or '?'}): wait for the save to "
                f"load, or click Continue in the game window. Then: cd {shlex.quote(str(play))} && claude")
    return f"Ready. Next: cd {shlex.quote(str(play))} && claude"


# ── launch ───────────────────────────────────────────────────────────────────

def launch_env(base: dict, bridge: Path, allow_unsupported: bool,
               native_wayland: bool) -> tuple[dict, list[str]]:
    """The game's environment, and one note per change worth telling the user."""
    env = dict(base)
    notes: list[str] = []
    pre = [p for p in env.get("LD_PRELOAD", "").replace(":", " ").split() if p != str(bridge)]
    env["LD_PRELOAD"] = ":".join([str(bridge)] + pre)
    if allow_unsupported:
        env["CK3_STRICT_BUILD_ID"] = "0"
    if env.get("WAYLAND_DISPLAY") and not env.get("SDL_VIDEODRIVER") and not native_wayland:
        env["SDL_VIDEODRIVER"] = "x11"
        del env["WAYLAND_DISPLAY"]
        notes.append("Wayland session: running the game under XWayland (--native-wayland to skip)")
    return env, notes


#: The entries of a CK3 user-data tree the launched game must OWN: the
#: directories it writes into and the files it rewrites. They start empty or
#: absent in the game's own tree. Every other file of the user's tree is copied
#: once (pdx_settings.txt, dlc_load.json: a symlink would carry the game's
#: write back), every other directory is symlinked (mod/: installed content).
#: The same rule as the research tree's instance.py PRIVATE_USER_DIR_*.
GAME_PRIVATE_DIRS = ("save games", "logs", "crashes", "dumps", "exceptions", "cache",
                     "shadercache", "player", "pops_filestorage")
GAME_PRIVATE_FILES = ("continue_game.json", "pops_filestorage.index", "last_save.ck3")


def userdir_ok(path: Path) -> bool:
    """CK3 splits its command line at spaces and quotes; an absolute path free of both."""
    s = str(path)
    return path.is_absolute() and not any(c.isspace() or c == '"' for c in s)


def prepare_game_dir(owner: Path, dst: Path) -> Path:
    """Build (once) the launched game's own user-data tree DST from the user's OWNER tree.

    OWNER is only read. Settings files are copied the first time, so the game's
    own settings writes stay in DST; a later change in OWNER is not picked up.
    """
    dst.mkdir(parents=True, exist_ok=True)
    private = set(GAME_PRIVATE_DIRS) | set(GAME_PRIVATE_FILES)
    if owner.is_dir():
        for entry in sorted(owner.iterdir()):
            if entry.name in private or entry.name.startswith("continue_game.json"):
                continue
            target = dst / entry.name
            if target.exists() or target.is_symlink():
                continue
            if entry.is_dir():
                target.symlink_to(entry)
            elif entry.is_file():
                with contextlib.suppress(OSError):
                    shutil.copy2(entry, target)
    for name in GAME_PRIVATE_DIRS:
        (dst / name).mkdir(exist_ok=True)
    return dst


#: The keys CK3 reads from continue_game.json under --continuelastsave, each
#: with no check that it is there: the reader (ck3+0x75b3dc0) looks up `title`,
#: `desc` and `date` and takes each value's string at +0x20, so a missing key
#: is a read of address 0x20 (ck3+0x80a6090) and the game dies before its main
#: menu. A pointer of `title` alone crashed 2026.12.1's every launch (node
#: smoke18; re/findings/re_userdir.md "The smoke18 crash").
CONTINUE_POINTER_KEYS = ("title", "desc", "date")


def complete_pointer(d: dict, title: str | None = None) -> tuple[dict, list[str]]:
    """continue_game.json content CK3 can read: D with every CONTINUE_POINTER_KEYS
    key a string. TITLE (a newly staged save) replaces the title and the two
    fields that describe it. Returns (pointer, the keys filled in)."""
    out = dict(d)
    filled: list[str] = []
    if title is not None:
        out.update(title=title, desc=title, date=time.strftime("%Y-%m-%d %H:%M:%S"))
    if not isinstance(out.get("title"), str) or not out["title"]:
        raise ValueError("continue_game.json names no save; pass --save PATH")
    if not isinstance(out.get("desc"), str):
        out["desc"] = out["title"]
        filled.append("desc")
    if not isinstance(out.get("date"), str):
        out["date"] = time.strftime("%Y-%m-%d %H:%M:%S")
        filled.append("date")
    return out, filled


def repair_pointer(ptr: Path) -> list[str]:
    """Fill the keys CK3 needs into the launched game's existing pointer (one
    2026.12.1 wrote holds only `title`). Returns the keys filled; [] if none."""
    pointer, filled = complete_pointer(_load_json(ptr))
    if filled:
        _write_json(ptr, pointer)
    return filled


def stage_save(path: Path) -> tuple[Path, str]:
    """Make PATH the save --continuelastsave loads in the launched game's own
    tree. Returns (save, sha256). PATH is copied there; it is never written.

    The game follows the `title` in continue_game.json, not file mtimes, so the
    game tree's pointer is rewritten, with every key CK3 reads from it
    (CONTINUE_POINTER_KEYS). The user's own pointer is not touched.
    """
    src = path.expanduser().resolve()
    if not src.is_file() or src.suffix != ".ck3":
        raise ValueError(f"{src}: not a .ck3 save file")
    saves = cfg.game_user_dir() / "save games"
    saves.mkdir(parents=True, exist_ok=True)
    dst = saves / src.name
    if dst.resolve() != src:
        if dst.exists() and sha256(dst) != sha256(src):
            raise ValueError(f"{dst} exists with different content (a save the launched game "
                             "wrote); rename your save, or move that file away")
        if not dst.exists():
            shutil.copy2(src, dst)
    now = time.time()
    os.utime(dst, (now, now))
    ptr = cfg.game_user_dir() / "continue_game.json"
    try:
        d = _load_json(ptr)
    except ValueError:
        d = {}
    _write_json(ptr, complete_pointer(d, dst.stem)[0])
    return dst, sha256(dst)


def owner_last_save() -> Path:
    """The save the user's own CK3 played last (their continue_game.json `title`)."""
    ptr = cfg.owner_game_dir() / "continue_game.json"
    try:
        title = _load_json(ptr).get("title")
    except (ValueError, OSError) as e:
        raise ValueError(f"{ptr}: {e}") from e
    if not isinstance(title, str) or not title:
        raise ValueError(f"{ptr} names no save; pass --save PATH")
    save = cfg.owner_game_dir() / "save games" / f"{title}.ck3"
    if not save.is_file():
        raise ValueError(f"{save} (named by {ptr}) does not exist; pass --save PATH")
    return save


def launch_argv(binary: Path, game_dir: Path, cont: bool, game_args: list[str]) -> list[str]:
    """The game's command line: its own user-data tree first (`-userdir=`)."""
    return [str(binary), f"-userdir={game_dir}"] + (["--continuelastsave"] if cont else []) \
        + list(game_args)


def _debug_log_hint() -> None:
    con.note(f"game log: {cfg.game_debug_log()}")


def shader_cache_empty(game_dir: Path) -> bool:
    """True when the game directory has no compiled shaders yet (a first start there)."""
    d = game_dir / "shadercache"
    try:
        return not any(d.iterdir())
    except OSError:
        return True


def launch_wait(no_wait: bool, wait: int | None, first_load: bool) -> int:
    """Seconds `launch` waits for the game: --wait, else FIRST_LOAD_WAIT for a
    launch whose shader cache is still to be built, else CONTINUE_WAIT. A plain
    launch (no save to load) waits for the main menu as long (L22, smoke19: it
    returned at once and a game that never reached the menu said nothing)."""
    if no_wait:
        return 0
    if wait is not None:
        return wait
    return FIRST_LOAD_WAIT if first_load else CONTINUE_WAIT


#: The game's own log line for each screen it enters ("Setting idler 'Frontend'").
_IDLER = re.compile(r"Setting idler '([^']+)'")

#: The screens that mean a plain launch reached the main menu (or went past it).
MENU_OR_LATER = ("Frontend", "Load Save", "In Game")

#: How far a debug.log's mtime may sit before the launch and still be this
#: launch's: the kernel stamps files from a coarse clock that can trail
#: time.time() by a tick. A previous run's log is minutes or days older.
LOG_CLOCK_SLACK = 2.0


@dataclass(frozen=True)
class GameStage:
    """What the game's debug.log says about this launch. kind is one of:
    "unreadable" (no log, or it cannot be read; `why` says which),
    "stale" (the log was last written before this launch: a previous run's),
    "no_idler" (this launch's log names no screen yet),
    "idler" (the last screen this launch entered, verbatim in `idler`)."""
    kind: str
    idler: str = ""
    why: str = ""


def game_stage(log: Path, launched: float) -> GameStage:
    """The last screen the game's debug.log says it entered since LAUNCHED (epoch s)."""
    try:
        mtime = log.stat().st_mtime
        text = log.read_text(errors="replace")
    except OSError as e:
        return GameStage("unreadable", why=e.strerror or str(e))
    if mtime < launched - LOG_CLOCK_SLACK:
        return GameStage("stale")
    hits = _IDLER.findall(text)
    return GameStage("idler", hits[-1]) if hits else GameStage("no_idler")


def not_active_hint(stage: GameStage, first_load: bool, log: Path, cont: bool = True) -> str:
    """What a launch that timed out (socket not up, or game not active) should tell
    the user. CONT: the launch asked the game to load a save (--continue/--save)."""
    ready = "`ck3agent doctor` says when it is ready."
    if stage.kind == "unreadable":
        return (f"The game's log {log} could not be read ({stage.why}), so what CK3 is doing is "
                f"not known. Leave it running; {ready}")
    if stage.kind == "stale":
        return (f"The game's log {log} predates this launch (CK3 has not written to it yet), so "
                f"it does not say what this game is doing. Leave it running; {ready}")
    if stage.kind == "no_idler":
        why = ("the first start in this game directory compiles the shader cache, which takes "
               "minutes" if first_load else "the game has not reached its main menu yet")
        return f"CK3 is still loading ({why}). Leave it running; {ready}"
    if stage.idler == "Frontend":
        if not cont:
            return "CK3 is at its main menu."
        return ("CK3 is at its main menu: the save did not load. A player save needs a click "
                "on Continue in the game window; otherwise see the game log.")
    if stage.idler == "In Game":
        return "CK3 is in game, but the bridge did not report it active. `ck3agent doctor` says why."
    if stage.idler == "Load Save":
        return f"CK3 is loading the save. {ready}"
    return f"CK3's last screen in its log is '{stage.idler}'. {ready}"


def busy_socket_lines(sock: str, holders: list[tuple[int, str]]) -> tuple[str, list[str]]:
    """The refusal when a bridge socket already answers: (detail, notes), naming its holders.

    Only a holder whose executable was READ as something other than `ck3` gets
    the note to end it. One whose executable could not be read is named as
    unidentified; a running game whose binary was replaced on disk reads as
    `.../ck3 (deleted)` and is still the game.
    """
    if not holders:
        return f"{sock}: another CK3 with the bridge is running (its process is not visible here)", []
    detail = f"{sock}: held by " + ", ".join(f"pid {pid} ({exe})" for pid, exe in holders)
    notes: list[str] = []
    game = False
    for pid, exe in holders:
        if exe == br.UNKNOWN_EXE:
            notes.append(f"pid {pid} could not be identified (its executable is not readable "
                         "here); check what it is before ending it.")
        elif Path(exe.removesuffix(" (deleted)")).name == "ck3":
            game = True
        else:
            notes.append(f"pid {pid} is not CK3: a process a crashed game started (its crash "
                         f"reporter) kept the socket open. End it (kill {pid}) and launch again.")
    if game:
        notes.append("another CK3 with the bridge is running")
    return detail, notes


def cmd_launch(a: argparse.Namespace) -> int:
    c = cfg.Config.load()
    binary = cfg.resolve("ck3_binary", c, a.ck3).path
    bridge = cfg.resolve("bridge_so", c, a.bridge).path
    if not binary or not binary.is_file() or not bridge or not bridge.is_file():
        con.fail("not configured", "run `ck3agent setup` first")
        return 1
    if not preload_ok(bridge):
        con.fail("bridge path unusable", f"{bridge}: LD_PRELOAD cannot hold ':' or spaces")
        return 1
    supported, why = check_build(binary)
    if not supported and not a.allow_unsupported:
        print(why)
        print("Refusing to launch. --allow-unsupported-build tries anyway.")
        return 1
    sock = br.socket_path()
    if br.alive(sock):
        detail, notes = busy_socket_lines(sock, br.socket_holders(sock))
        con.fail("a bridge is already listening", detail)
        for n in notes:
            con.note(n)
        return 1

    if any(x.lstrip("-").startswith("userdir") for x in a.game_args):
        con.fail("-userdir in the game arguments",
                 "ck3agent gives the game its own user-data directory; set XDG_STATE_HOME to move it")
        return 1
    gdir = cfg.game_user_dir().resolve()
    if not userdir_ok(gdir):
        con.fail("game directory unusable", f"{gdir}: CK3 cannot take a path with spaces or quotes; "
                 "set XDG_STATE_HOME to one without")
        return 1
    cont = a.cont or bool(a.save)
    record = {"launched_utc": _now_utc(), "binary": str(binary), "bridge": str(bridge),
              "game_user_dir": str(gdir)}
    try:
        prepare_game_dir(cfg.owner_game_dir(), gdir)
        src = Path(a.save) if a.save else None
        if src is None and cont and not (gdir / "continue_game.json").exists():
            src = owner_last_save()     # first launch: the save the user's CK3 played last
        if src is not None:
            save, digest = stage_save(src)
            con.ok("save staged", f"{save} (sha256 {digest[:12]})")
            record["save"], record["save_sha256"] = str(save), digest
        elif cont or (gdir / "continue_game.json").exists():
            # Every launch, not only --continue: the main menu's Continue may read
            # the same pointer, and a title-only one 2026.12.1 left behind crashes
            # its reader (re_userdir.md). A plain launch loads no save, so a
            # pointer naming none is left as it is there.
            try:
                filled = repair_pointer(gdir / "continue_game.json")
            except ValueError:
                if cont:
                    raise
                filled = []
            if filled:
                con.ok("continue_game.json repaired", f"added {', '.join(filled)} (CK3 crashes on load "
                       "without them)")
    except (ValueError, OSError) as e:
        con.fail("--save" if a.save else "game directory", str(e))
        return 1
    con.ok("game directory", f"{gdir} (saves land in its save games/)")
    first_load = shader_cache_empty(gdir)
    wait = launch_wait(a.no_wait, a.wait, first_load)
    if first_load and wait:
        con.note(f"first start in this game directory: CK3 builds its shader cache (about 600 MB, "
                 f"several minutes); waiting up to {wait}s")

    env, notes = launch_env(os.environ, bridge, a.allow_unsupported, a.native_wayland)
    for n in notes:
        con.note(n)
    argv = launch_argv(binary, gdir, cont, a.game_args)
    logs = _logs_dir()
    logs.mkdir(parents=True, exist_ok=True)
    out = logs / f"ck3-{datetime.now():%Y%m%d-%H%M%S}.log"
    launched = time.time()
    with out.open("wb") as fh:
        proc = subprocess.Popen(argv, cwd=binary.parent, env=env, stdout=fh, stderr=subprocess.STDOUT,
                                start_new_session=True)
    record.update(pid=proc.pid, argv=argv, output=str(out))
    _write_json(_state_file(), record)
    print(f"CK3 pid {proc.pid}; output {out}")
    print(f"bridge log {br.bridge_log_path()}")
    if not wait:
        return 0

    deadline = time.time() + wait
    while time.time() < deadline:
        if proc.poll() is not None:
            con.fail("CK3 exited early", f"code {proc.returncode}; see {out}")
            _debug_log_hint()
            return 1
        if br.alive(sock):
            con.ok("bridge socket up", sock)
            break
        time.sleep(1.0)
    else:
        hit = br.find_refusal(Path(br.bridge_log_path()), None)
        if hit:
            con.fail("bridge refused to load", hit[0])
            _debug_log_hint()
            return 1
        con.warn("bridge socket not up", f"after {wait}s. `ck3agent doctor` shows why.")
        log = cfg.game_debug_log()
        con.note(not_active_hint(game_stage(log, launched), first_load, log, cont))
        _debug_log_hint()
        return EXIT_TIMEOUT
    if cont:
        rc = br.wait_for_active(sock, deadline, proc, pause=not a.no_pause)
        if rc == EXIT_TIMEOUT:
            log = cfg.game_debug_log()
            con.note(not_active_hint(game_stage(log, launched), first_load, log))
        if rc:
            _debug_log_hint()
        return rc
    return wait_for_menu(proc, deadline, launched, first_load, wait, out)


def wait_for_menu(proc, deadline: float, launched: float, first_load: bool, wait: int,
                  out: Path) -> int:
    """A plain launch: wait until the game's log says it reached the main menu.
    The bridge socket is up long before the menu, so the socket alone says
    nothing about whether the game will get there (smoke19: a headless host
    sat after "End loading of history" for 600 s)."""
    log = cfg.game_debug_log()
    while True:
        stage = game_stage(log, launched)
        if stage.kind == "idler" and stage.idler in MENU_OR_LATER:
            con.ok("game at its main menu" if stage.idler == "Frontend" else "game past its main menu",
                   f"its log: '{stage.idler}'")
            return 0
        if proc.poll() is not None:
            con.fail("CK3 exited early", f"code {proc.returncode}; see {out}")
            _debug_log_hint()
            return 1
        if time.time() >= deadline:
            break
        time.sleep(1.0)
    con.warn("game not at its main menu", f"after {wait}s")
    con.note(not_active_hint(stage, first_load, log, cont=False))
    _debug_log_hint()
    return EXIT_TIMEOUT


# ── serve ────────────────────────────────────────────────────────────────────

def cmd_serve(a: argparse.Namespace) -> int:
    if a.transport == "http" and a.host not in LOOPBACK and not a.allow_remote:
        print(f"refusing --host {a.host}: the server has no authentication. "
              "Use an ssh tunnel, or pass --allow-remote.", file=sys.stderr)
        return 2
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s", stream=sys.stderr)
    bootstrap_server()
    import mcp_server  # noqa
    if a.transport == "stdio":
        mcp_server.mcp.run(transport="stdio")
        return 0
    import getpass
    import socket
    mcp_server.mcp.settings.host = a.host
    mcp_server.mcp.settings.port = a.port
    url = f"http://127.0.0.1:{a.port}/mcp"
    snippet = {"mcpServers": {MCP_SERVER_NAME: {"command": "npx", "args": ["-y", "mcp-remote", url]}}}
    print(f"ck3agent MCP server on http://{a.host}:{a.port}/mcp (no auth)\n"
          f"claude_desktop_config.json:\n{json.dumps(snippet, indent=2)}\n"
          f"From another machine, tunnel first:\n"
          f"  ssh -N -L {a.port}:127.0.0.1:{a.port} {getpass.getuser()}@{socket.gethostname()}",
          file=sys.stderr)
    mcp_server.mcp.run(transport="streamable-http")
    return 0


# ── run ──────────────────────────────────────────────────────────────────────

def parse_date(s: str) -> tuple[int, int, int]:
    try:
        y, m, d = (int(x) for x in str(s).strip().split("."))
    except ValueError:
        raise argparse.ArgumentTypeError(f"{s!r}: expected YYYY.M.D") from None
    return y, m, d


def claude_supports(claude: str, flag: str) -> bool:
    try:
        out = subprocess.run([claude, "--help"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return False
    return flag in (out.stdout + out.stderr)


def claude_version(claude: str) -> str | None:
    try:
        out = subprocess.run([claude, "--version"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() or None


def build_claude_argv(claude: str, prompt: str, mcp_config: Path, a: argparse.Namespace,
                      strict: bool) -> list[str]:
    # The prompt goes right after -p: --mcp-config and --allowedTools take lists.
    argv = [claude, "-p", prompt, "--output-format", "stream-json", "--verbose",
            "--mcp-config", str(mcp_config)]
    if strict:
        argv.append("--strict-mcp-config")
    if a.model:
        argv += ["--model", a.model]
    if a.max_turns:
        argv += ["--max-turns", str(a.max_turns)]
    if a.skip_permissions:
        argv.append("--dangerously-skip-permissions")
    else:
        argv += ["--allowedTools", *ALLOW]
    return argv + list(a.claude_args)


#: One definition of a cycle, for the manifest and every reader of it (L18-C: the
#: pilots' hand analyzers counted groups of waits, called an 18-cycle seat whose
#: last cycle ended in `pause` "ended_early_success", and smoke16 read a
#: `cycles_completed` the manifest never had).
CYCLE_DEFINITION = ("a cycle is a stretch of the transcript between two `session(action=\"wait\")` "
                    "groups (consecutive waits count once; the run's start and end bound the first "
                    "and last stretch) that contains at least one `observe` call")


class CycleCounter:
    """CYCLE_DEFINITION, fed one stream-json line at a time by the transcript
    drain. ONE counter for the run's cycle stop and the manifest (L18 round 3):
    `_Watch` halts the run when `closed` reaches the cycles asked for and then
    freezes this counter, and the manifest's `cycles_completed` is `result()` of
    this same object. A line claude writes after the halt (a call that was in
    flight when the stop fired) is in the transcript but not in the count;
    `count_cycles` re-reads the whole transcript as the manifest's
    `cycles_recounted` cross-check, which can exceed it by those lines."""

    PREFIX = f"mcp__{MCP_SERVER_NAME}__"

    def __init__(self) -> None:
        self.calls: list[str] = []
        self.closed = 0          # cycles a wait group has ended
        self.groups = 0
        self._observed = self._waiting = False
        self.frozen: dict | None = None

    def freeze(self) -> dict:
        """Stop counting: `result()` is this reading from now on. Called by the
        stop that read `closed`, so the recorded count is the one it acted on."""
        if self.frozen is None:
            self.frozen = self.result()
        return self.frozen

    def feed(self, line) -> None:
        if self.frozen is not None:
            return
        try:
            ev = json.loads(line)
        except (ValueError, TypeError):
            return
        if not isinstance(ev, dict) or ev.get("type") != "assistant":
            return
        for b in (ev.get("message") or {}).get("content") or []:
            if not isinstance(b, dict) or b.get("type") != "tool_use":
                continue
            name = b.get("name") or ""
            if not name.startswith(self.PREFIX):
                continue
            name = name[len(self.PREFIX):]
            if name == "session":
                name = f"session:{(b.get('input') or {}).get('action')}"
            self._call(name)

    def _call(self, name: str) -> None:
        self.calls.append(name)
        if name == "session:wait":
            if not self._waiting:
                self.groups += 1
                self.closed += self._observed
                self._observed = False
            self._waiting = True
            return
        self._waiting = False
        self._observed = self._observed or name == "observe"

    def result(self) -> dict:
        if self.frozen is not None:
            return dict(self.frozen)
        return {"cycles_completed": self.closed + self._observed,
                "observe_calls": self.calls.count("observe"),
                "wait_groups": self.groups, "ended_on": self.calls[-1] if self.calls else None}


def count_cycles(lines) -> dict:
    """The run's cycles under CYCLE_DEFINITION, re-read from its stream-json
    transcript: a reader's recount (the manifest's `cycles_recounted`), not the
    counter the run's stop read (CycleCounter, which the manifest records).

    Returns {"cycles_completed", "observe_calls", "wait_groups", "ended_on"}:
    `ended_on` is the last ck3 call's name ("session:pause", "session:wait",
    ...) or None when the run made none.
    """
    c = CycleCounter()
    for line in lines:
        c.feed(line)
    return c.result()


def summarize_stream(lines) -> dict:
    """Pull the init and result events out of stream-json lines."""
    m: dict = {}
    for line in lines:
        try:
            ev = json.loads(line)
        except (ValueError, TypeError):
            continue
        if not isinstance(ev, dict):
            continue
        if ev.get("type") == "system" and ev.get("subtype") == "init":
            m.update(model=ev.get("model"), session_id=ev.get("session_id"),
                     cli_version=ev.get("claude_code_version"),
                     permission_mode=ev.get("permissionMode"),
                     mcp_servers=ev.get("mcp_servers"), init_tools=ev.get("tools"))
        elif ev.get("type") == "result":
            m.update(result=ev.get("subtype"), is_error=ev.get("is_error"),
                     num_turns=ev.get("num_turns"), total_cost_usd=ev.get("total_cost_usd"),
                     usage=ev.get("usage"), model_usage=ev.get("modelUsage"),
                     duration_ms=ev.get("duration_ms"))
            m["session_id"] = m.get("session_id") or ev.get("session_id")
            m["result_event"] = True
    return m


def _echo(line: str) -> None:
    try:
        ev = json.loads(line)
    except ValueError:
        return
    if not isinstance(ev, dict):
        return
    if ev.get("type") == "assistant":
        for b in (ev.get("message") or {}).get("content") or []:
            if b.get("type") == "text" and b.get("text"):
                con.say(b["text"].rstrip())
            elif b.get("type") == "tool_use":
                con.say(f"  > {b.get('name')}")
    elif ev.get("type") == "result":
        con.say(f"[{ev.get('subtype')}] turns {ev.get('num_turns')}, cost ${ev.get('total_cost_usd')}")


#: Seconds claude gets after SIGINT to end its stream, then after SIGTERM, before the next signal.
STOP_GRACE = 30.0
TERM_GRACE = 10.0
#: Seconds to wait for the MCP server to let go of its bridge session after claude exits.
RELEASE_WAIT = 15.0
UNRECORDED = ("result", "num_turns", "total_cost_usd", "usage", "model_usage", "duration_ms")


def _stop_claude(proc, interrupted: bool = False) -> str:
    """SIGINT so claude can end its stream, then SIGTERM, then SIGKILL.

    Returns the last signal sent, or 'exited' if claude was already gone.
    """
    how = "SIGINT" if interrupted else "exited"
    for sig, grace in ((signal.SIGINT, STOP_GRACE), (signal.SIGTERM, TERM_GRACE),
                       (signal.SIGKILL, TERM_GRACE)):
        if proc.poll() is not None:
            return how
        if not (interrupted and sig == signal.SIGINT):
            proc.send_signal(sig)
        how = sig.name
        try:
            proc.wait(grace)
            return how
        except subprocess.TimeoutExpired:
            pass
    return how


class _Watch(threading.Thread):
    """Stops a run at its cycle count, a game date or after N wall seconds:
    interrupt claude, pause, wait.

    The cycle count is the harness's, not the prompt's (L18 round 2, owner
    ruling): `counter` is the CycleCounter the transcript drain feeds, and the
    run stops when it has closed `cycles` cycles. One control session for the
    whole run, not one per poll: `link`, shared with `ck3agent run`, which
    closes it.
    """

    POLL = 15.0

    def __init__(self, proc, sock: str, until, wall: int | None, link: br.Link | None = None,
                 cycles: int = 0, counter: "CycleCounter | None" = None):
        super().__init__(daemon=True)
        self.proc, self.sock, self.until = proc, sock, until
        self.cycles, self.counter = cycles, counter
        self._own = link is None
        self.link = link or br.Link(sock, mode="control")
        self.deadline = time.monotonic() + wall if wall else None
        self.reason: str | None = None
        self.how: str | None = None
        self.done = threading.Event()

    def run(self) -> None:
        try:
            self._poll()
        finally:
            if self._own:
                self.link.close()

    def _poll(self) -> None:
        next_date = 0.0
        while not self.done.wait(1.0):
            if self.cycles and self.counter is not None and self.counter.closed >= self.cycles:
                return self._halt("cycles")
            if self.deadline and time.monotonic() >= self.deadline:
                return self._halt("wall_seconds")
            if self.until and time.monotonic() >= next_date:
                next_date = time.monotonic() + self.POLL
                d = br._date(br._tick(self.link.call("get_tick_info")))
                try:
                    if d and parse_date(d) >= self.until:
                        return self._halt("until_date")
                except argparse.ArgumentTypeError:
                    pass

    def _halt(self, reason: str) -> None:   # not _stop: Thread.join() calls Thread._stop on 3.11/3.12
        # The count the stop acted on is the count the manifest records: freeze
        # it before claude can write another call (L18 round 3).
        self.reason = reason
        if self.counter is not None:
            self.counter.freeze()
        # SIGINT first: claude stops issuing tool calls, so the pause below holds.
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
        self.link.call("pause_game")
        self.how = _stop_claude(self.proc, interrupted=True)


class _Drain(threading.Thread):
    """claude's stdout to the transcript (and the terminal), line by line."""

    def __init__(self, src, fh, echo: bool, counter: "CycleCounter | None" = None):
        super().__init__(daemon=True)
        self.src, self.fh, self.echo, self.counter = src, fh, echo, counter

    def run(self) -> None:
        with contextlib.suppress(ValueError, OSError):  # fh closed under us: a grandchild kept the pipe
            self._copy()

    def _copy(self) -> None:
        for line in self.src:
            self.fh.write(line)
            self.fh.flush()
            if self.counter is not None:
                self.counter.feed(line)
            if self.echo:
                try:
                    _echo(line)
                except Exception:  # noqa: BLE001  (an odd event shape must not stop the transcript)
                    pass


class _Signalled(Exception):
    def __init__(self, signum: int):
        self.signum = signum
        super().__init__(signal.Signals(signum).name)


class _Gate:
    """The run's signal handler. While `held`, a signal is recorded instead of
    raised: raised inside subprocess.Popen it would leave claude running with no
    `proc` to stop. SIGINT raises KeyboardInterrupt, as Python's own handler does."""

    def __init__(self) -> None:
        self.held = False
        self.pending: int | None = None

    def __call__(self, signum, _frame):
        if self.held:
            self.pending = self.pending or signum
            return
        self._raise(signum)

    @staticmethod
    def _raise(signum: int):
        if signum == signal.SIGINT:
            raise KeyboardInterrupt
        raise _Signalled(signum)

    def release(self) -> None:
        self.held = False
        if self.pending:
            self._raise(self.pending)


def _raise_signalled(signum, _frame):
    raise _Signalled(signum)


def _set_signals(handler, with_int: bool | None = None) -> dict:
    """Install `handler` for SIGTERM and SIGHUP, and SIGINT if `with_int` (default:
    only for SIG_IGN); return the old ones."""
    if with_int is None:
        with_int = handler is signal.SIG_IGN
    old = {}
    sigs = (signal.SIGTERM, signal.SIGHUP) + ((signal.SIGINT,) if with_int else ())
    for s in sigs:
        try:
            old[s] = signal.signal(s, handler)
        except (ValueError, OSError):  # not the main thread
            pass
    return old


def _copy_range(src: Path, start: int, dst: Path) -> dict:
    try:
        size = src.stat().st_size
    except OSError:
        return {"source": str(src), "bytes": 0, "note": "no action log"}
    note = None
    if size < start:
        start, note = 0, "log shrank during the run; copied whole file"
    with src.open("rb") as f:
        f.seek(start)
        data = f.read(size - start)
    dst.write_bytes(data)
    out = {"source": str(src), "offset": start, "bytes": len(data), "lines": data.count(b"\n")}
    if note:
        out["note"] = note
    return out


def _wait_for_record(path: Path, wait: float) -> dict | None:
    deadline = time.monotonic() + wait
    while True:
        try:
            return _load_json(path) or None
        except (ValueError, OSError):
            pass  # absent ({} -> None above) or half-written
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.5)


def _launch_check(launch: dict | None, sock: str) -> dict:
    """Whether the last `ck3agent launch` record describes the game now serving the socket."""
    rec = (launch or {}).get("pid")
    live = br.peer_pid(sock)
    return {"recorded_pid": rec, "bridge_pid": live,
            "matches": None if rec is None or live is None else rec == live}


def _claim_stamp(runs: Path, now: datetime | None = None) -> str:
    """A run stamp no other run in `runs` has: the second, then -2, -3, ...

    Claimed by creating <stamp>.prompt.md exclusively, so two runs started in
    the same second cannot share one.
    """
    base = (now or datetime.now()).strftime("%Y%m%d-%H%M%S")
    n = 1
    while True:
        stamp = base if n == 1 else f"{base}-{n}"
        try:
            os.close(os.open(runs / f"{stamp}.prompt.md", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644))
            return stamp
        except FileExistsError:
            n += 1


def _release_after(paths: dict, sock: str, link: br.Link) -> tuple[dict | None, dict | None]:
    """(the MCP server's release record, ours from resuming its session).

    The server writes its session id and resume token at handshake. With that
    record, this resumes the session once the server has let go of it and
    releases whatever it still holds, so the release does not depend on the
    server outliving Claude Code's 500 ms stop window.
    """
    try:
        sess = _load_json(paths["session"])
    except (ValueError, OSError):
        sess = {}
    resumed = None
    if sess:
        resumed = br.release_session(sock, sess, link, RELEASE_WAIT)
        with contextlib.suppress(OSError):
            paths["session"].unlink()          # the token is a credential for that session
        wait = 0.0                             # the server has let go: its record is written or never will be
    else:
        # No session record: the server never got a bridge session, so its release record says why.
        wait = RELEASE_WAIT if paths["tools"].exists() else 0.0
    return _wait_for_record(paths["release"], wait), resumed


def _clean(rec: dict | None) -> bool:
    return bool(rec) and not rec.get("error") and not rec.get("refused")


#: The bridge's refusal to empty a control session (dispatch_router.cpp, ck-cwal).
#: Not a failed release: in free play the session's close frees that ruler, and
#: the teardown's ruler check below is what verifies it.
LAST_CLAIM = "last_claim_of_control_session"


def _kept_as_last_claim(rec: dict | None) -> list[str]:
    """The rulers `rec` left bound only because each was its session's last claim.

    Empty unless that is ALL it refused and it has no error: any other refusal
    or an error is still a warning.
    """
    refused = (rec or {}).get("refused") or {}
    if rec and not rec.get("error") and refused and set(refused.values()) == {LAST_CLAIM}:
        return list(refused)
    return []


def _report_release(what: str, rec: dict, detail: str) -> None:
    kept = _kept_as_last_claim(rec)
    if kept:
        done = f"released {rec.get('released')}; " if rec.get("released") else ""
        con.ok(what, f"{done}last ruler {', '.join(kept)} stays bound until the session closes "
                     "(the bridge refuses to empty a control session); checked below")
    else:
        (con.ok if _clean(rec) else con.warn)(what, detail)


def _declared_rulers(raw: str | None) -> list[int] | str | None:
    """CK3_AGENT_CHARACTER_ID as the server reads it: ids in order, or the raw text
    when the server would refuse it (it then binds nothing), or None when unset."""
    if raw is None or not raw.strip():
        return None
    ids: list[int] = []
    for part in raw.split(","):
        try:
            cid = int(part.strip())
        except ValueError:
            return raw
        if cid <= 0:
            return raw
        if cid not in ids:
            ids.append(cid)
    return ids if len(ids) <= 10 else raw


def _rulers_record(declared, prebind, release: dict | None, resumed: dict | None,
                   after: list[int] | None, wait: dict | None = None) -> dict:
    """Every ruler of the run: declared, bound at start (and WHEN: the tick and
    date each bind landed, `bound_at`), held at stop, held after teardown.
    `prebind_wait` is how long the server waited for the game to be active
    before binding (`waited_s`, `polls`, `game_active`); None when the server
    did not say."""
    at_stop: list[int] = []
    for rec in (release, resumed):
        for c in (rec or {}).get("claims") or []:
            if c not in at_stop:
                at_stop.append(c)
    return {"declared": declared,
            "bound_at_start": [p["character_id"] for p in prebind or [] if p.get("ok")],
            "bound_at": {str(p["character_id"]): p.get("bound_at")
                         for p in prebind or [] if p.get("ok")},
            "refused_at_start": {str(p["character_id"]): p.get("reason")
                                 for p in prebind or [] if not p.get("ok")},
            "prebind_wait": dict(wait) if isinstance(wait, dict) else None,
            "held_at_stop": at_stop, "held_after_teardown": after}


def _bound_when(rl: dict) -> str:
    """For the run report: the date(s) the binds landed and the wait, if known."""
    dates: list[str] = []
    for v in (rl.get("bound_at") or {}).values():
        d = (v or {}).get("date") if isinstance(v, dict) else None
        if d and d not in dates:
            dates.append(d)
    waited = (rl.get("prebind_wait") or {}).get("waited_s")
    out = f" on {', '.join(dates)}" if dates else ""
    if isinstance(waited, (int, float)) and waited > 0:
        out += f" after waiting {waited:.0f} s for the game"
    return out


def cmd_run(a: argparse.Namespace) -> int:
    claude = shutil.which("claude")
    if not claude:
        con.fail("claude CLI not on PATH", "install Claude Code: https://docs.claude.com/en/docs/claude-code")
        return 1
    c = cfg.Config.load()
    play = cfg.resolve("play_dir", c, a.play_dir).path
    try:
        mcp = _load_json(play / ".mcp.json")
    except ValueError as e:
        con.fail(".mcp.json unreadable", str(e))
        return 1
    if MCP_SERVER_NAME not in mcp.get("mcpServers", {}):
        con.fail("play directory not set up", f"{play}: run `ck3agent setup`")
        return 1
    sock = br.socket_path()
    if not br.alive(sock) and not a.no_game_check:
        con.fail("bridge socket not listening", f"{sock}: start the game first (`ck3agent launch --continue`)")
        return 1
    if a.skip_permissions:
        print("warning: --dangerously-skip-permissions lets Claude run any tool unasked. "
              "Game text reaches the model; a crafted save or mod could steer it.", file=sys.stderr)

    prompt = Path(a.prompt_file).read_text(encoding="utf-8") if a.prompt_file else \
        (PROTOCOL_DIR / "run-prompt.md").read_text(encoding="utf-8")
    prompt = prompt.replace("{{goal}}", a.goal or "Play well: survive, grow the realm, keep the succession intact.")
    # The shipped run prompt names no cycle count: the run counts and stops
    # (_Watch). A --prompt-file may still say {{cycles}}.
    prompt = prompt.replace("{{cycles}}", str(a.cycles))

    runs = play / "runs"
    runs.mkdir(exist_ok=True)
    stamp = _claim_stamp(runs)
    base = runs / stamp
    paths = {k: Path(f"{base}{ext}") for k, ext in (
        ("prompt", ".prompt.md"), ("stream", ".jsonl"), ("manifest", ".json"), ("stderr", ".stderr.log"),
        ("tools", ".tools.json"), ("actions", ".actions.jsonl"), ("mcp", ".mcp.json"),
        ("release", ".release.json"), ("session", ".session.json"))}
    paths["prompt"].write_text(prompt, encoding="utf-8")
    srv = {**dict(mcp["mcpServers"][MCP_SERVER_NAME]), **MCP_ALWAYS_LOAD}
    srv["env"] = {**(srv.get("env") or {}), "CK3_TOOL_SURFACE_OUT": str(paths["tools"]),
                  "CK3_RELEASE_ON_EXIT": str(paths["release"]), "CK3_SESSION_OUT": str(paths["session"])}
    _write_json(paths["mcp"], {"mcpServers": {MCP_SERVER_NAME: srv}})

    argv = build_claude_argv(claude, prompt, paths["mcp"], a, claude_supports(claude, "--strict-mcp-config"))
    # Claude Code moves an MCP call still running after 120 s into a background task
    # (CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS); session(action="wait") blocks for up to
    # that long by design, so the run turns the hand-off off. Setup writes the same
    # variable into the play directory's settings for interactive sessions.
    # CK3_SESSION_OUT is also this seat's identity for the live-state hook, which
    # claude runs as its child: the hook reads its ruler from THIS session's record
    # and never from the bridge's global primary, another seat's (L18-C).
    env = dict(os.environ, CK3_TOOL_SURFACE_OUT=str(paths["tools"]),
               CK3_SESSION_OUT=str(paths["session"]),
               CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS="0")
    action_log = Path(br.action_log_path())
    try:
        log_start = action_log.stat().st_size
    except OSError:
        log_start = 0
    # One control session serves the start date, the watcher, the release check, the
    # end date and the teardown. Closed between the start and the end unless the
    # watcher holds it: an idle session collects the bridge's pings for hours.
    link = br.Link(sock, mode="control")
    manifest: dict = {
        "ck3agent_version": __version__, "supported_build_id": SUPPORTED_BUILD_ID,
        "started_utc": _now_utc(), "play_dir": str(play), "cycles": a.cycles, "goal": a.goal,
        "prompt_file": str(paths["prompt"]), "max_turns": a.max_turns,
        "skip_permissions": a.skip_permissions, "until_date": a.until_date,
        "wall_seconds": a.wall_seconds, "argv": ["<prompt>" if x == prompt else x for x in argv],
        "start_game_date": br.game_date(sock, link),
    }
    link.close()
    try:
        manifest["launch"] = _load_json(_state_file()) or None
    except ValueError:
        manifest["launch"] = None
    manifest["launch_check"] = _launch_check(manifest["launch"], sock)
    if manifest["launch_check"]["matches"] is False:
        con.warn("launch record", f"pid {manifest['launch_check']['recorded_pid']}, bridge pid "
                 f"{manifest['launch_check']['bridge_pid']}: it may describe another game")
    print(f"run {stamp}: {a.cycles} cycles, max {a.max_turns} turns; transcript {paths['stream']}")

    rc, stop, how = 1, None, None
    proc = watch = drain = None
    counter = CycleCounter()
    gate = _Gate()
    fh = paths["stream"].open("w", encoding="utf-8")
    err = paths["stderr"].open("w", encoding="utf-8")
    old_signals = _set_signals(gate, with_int=signal.getsignal(signal.SIGINT) is signal.default_int_handler)
    try:
        gate.held = True
        try:
            proc = subprocess.Popen(argv, cwd=play, env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=err, encoding="utf-8", errors="replace")
        finally:
            gate.held = False
        gate.release()
        drain = _Drain(proc.stdout, fh, echo=not a.quiet, counter=counter)
        drain.start()
        if a.until_date or a.wall_seconds or a.cycles:
            watch = _Watch(proc, sock, a.until_date, a.wall_seconds, link,
                           cycles=a.cycles, counter=counter)
            watch.start()
        rc = proc.wait()
    except OSError as e:
        con.fail("cannot start claude", str(e))
    except KeyboardInterrupt:
        stop, rc = "interrupted", 130
    except _Signalled as e:
        stop, rc = f"signal {e}", 128 + e.signum
    finally:
        old_signals = {**_set_signals(signal.SIG_IGN, with_int=True), **old_signals}
        try:
            if gate.pending and not stop:          # Popen failed with a signal held
                stop, rc = f"signal {signal.Signals(gate.pending).name}", 128 + gate.pending
            # Nothing reaches the terminal until the manifest is written: after a
            # hangup every write fails, and must not cost the teardown or the record.
            if watch:
                watch.done.set()
                watch.join(STOP_GRACE + 2 * TERM_GRACE + 20)
                stop = stop or watch.reason
                how = watch.how
            if proc:
                how = how if how and how != "exited" else _stop_claude(proc, interrupted=stop == "interrupted")
            if drain:
                drain.join(10)
            fh.close()
            err.close()
            if stop in ("cycles", "until_date", "wall_seconds"):
                rc = 0
            try:
                srec = _load_json(paths["session"]) or {}
                prebind, prebind_wait = srec.get("prebind"), srec.get("prebind_wait")
            except (ValueError, OSError):
                srec, prebind, prebind_wait = {}, None, None
            release, resumed = _release_after(paths, sock, link)
            good = _clean(resumed) if resumed is not None else _clean(release)
            # L20-B: this run's sessions only (the server's, from its session
            # record); another run's rulers are named and left alone.
            own_sessions = [s for s in ((srec if isinstance(srec, dict) else {}).get("session_id"),
                                        (resumed or {}).get("session_id")) if s]
            teardown = br.teardown(sock, wait=0 if good else 5.0, link=link, sessions=own_sessions)
            with paths["stream"].open(encoding="utf-8", errors="replace") as f:
                manifest.update(summarize_stream(f))
            # The one counter: the drain fed it, the cycle stop read it (and froze
            # it); the recount of the transcript is the cross-check beside it.
            manifest.update(counter.result(), cycle_definition=CYCLE_DEFINITION)
            with paths["stream"].open(encoding="utf-8", errors="replace") as f:
                manifest["cycles_recounted"] = count_cycles(f)["cycles_completed"]
            # Asked for, and met: one comparison, here, so no reader re-derives it.
            manifest["cycles_met"] = manifest["cycles_completed"] >= a.cycles
            # The cost is Claude Code's own estimate from its result event (list
            # prices; it includes the side model in model_usage), not a bill.
            manifest["total_cost_usd_source"] = ("claude result event (client-side estimate)"
                                                 if manifest.get("result_event") else None)
            if not manifest.get("result_event"):
                why = "terminated" if how not in (None, "exited") else "no result event"
                manifest["result_event"] = False
                for k in UNRECORDED:
                    manifest[k] = f"unrecorded: {why}"
            manifest["cli_version"] = manifest.get("cli_version") or claude_version(claude)
            manifest.update(
                ended_utc=_now_utc(), exit_code=rc, stop_reason=stop, claude_stop=how,
                end_game_date=br.game_date(sock, link), server_release=release, resumed_release=resumed,
                teardown=[{"step": w, "ok": g, "detail": d} for w, g, d in teardown],
                tool_surface=_load_tool_surface(paths["tools"]),
                actions=_copy_range(action_log, log_start, paths["actions"]),
                rulers=_rulers_record(_declared_rulers(os.environ.get("CK3_AGENT_CHARACTER_ID")),
                                      prebind, release, resumed, br.held_rulers(sock, link),
                                      wait=prebind_wait))
            link.close()
            manifest["bridge_sessions_opened"] = link.opened
            _write_json(paths["manifest"], manifest)

            if release:
                _report_release(
                    "MCP server release", release, f"released {release.get('released')}, refused "
                    f"{release.get('refused') or 'none'}, {release.get('error') or 'no error'}")
            if resumed and resumed.get("resume_status") == "session_gone":
                # Nothing was resumed: the bridge had already dropped the session, and its claims with it.
                (con.ok if _clean(resumed) else con.warn)(
                    "session release", f"session {resumed.get('session_id')} already closed, nothing held")
            elif resumed:
                _report_release(
                    "session release", resumed, f"resumed {resumed.get('session_id')}: {resumed.get('claims')} "
                    f"held, released {resumed.get('released')}, refused {resumed.get('refused') or 'none'}, "
                    f"{resumed.get('error') or 'no error'}")
            for what, ok, detail in teardown:
                (con.ok if ok else con.warn)(f"teardown {what}", detail)
            rl = manifest["rulers"]
            if rl["bound_at_start"] or rl["refused_at_start"] or rl["declared"] is not None:
                (con.warn if rl["refused_at_start"] or isinstance(rl["declared"], str) else con.ok)(
                    "rulers", f"bound at start {rl['bound_at_start'] or 'none'}"
                    + (_bound_when(rl) if rl["bound_at_start"] else "")
                    + (f", refused {rl['refused_at_start']}" if rl["refused_at_start"] else "")
                    + (f" (CK3_AGENT_CHARACTER_ID={rl['declared']!r} is not an id list)"
                       if isinstance(rl["declared"], str) else "")
                    + f"; held at stop {rl['held_at_stop'] or 'none'}")
            con.say(f"run {stamp} ended (code {rc}{', ' + stop if stop else ''}); manifest {paths['manifest']}")
        finally:
            for s, h in old_signals.items():
                with contextlib.suppress(ValueError, OSError, TypeError):
                    signal.signal(s, h)
    return rc


def _load_tool_surface(path: Path) -> dict | None:
    """The server's own record: path, tool names, schema hash. Schemas stay in the file."""
    try:
        d = _load_json(path)
    except (ValueError, OSError):
        return {"path": str(path), "error": "unreadable"}
    if not d:
        return None
    return {"path": str(path), "tools": d.get("tools"), "schemas_via": d.get("schemas_via"),
            "schemas_sha256": d.get("schemas_sha256")}


# ── hidden: hook-state, version ──────────────────────────────────────────────

def cmd_hook_state(a: argparse.Namespace) -> int:
    import runpy
    hook = Path(__file__).resolve().parent / "hooks" / "inject_state.py"
    sys.argv = [str(hook)]
    bootstrap_server()
    try:
        runpy.run_path(str(hook), run_name="__main__")
    except SystemExit as e:
        return int(e.code or 0)
    return 0


def cmd_version(a: argparse.Namespace) -> int:
    print(VERSION_LINE)
    return 0


# ── config ───────────────────────────────────────────────────────────────────

def cmd_config(a: argparse.Namespace) -> int:
    c = cfg.Config.load()
    p = cfg.config_path()
    rows = [("config file", str(p), "exists" if p.exists() else "not written yet")]
    flags = {"ck3_binary": a.ck3, "bridge_so": a.bridge, "play_dir": a.play_dir}
    for name in ("ck3_binary", "bridge_so", "play_dir"):
        r = cfg.resolve(name, c, flags[name])
        rows.append((name, str(r.path) if r.path else "-", r.source))
    rows += [
        ("data dir", str(cfg.data_dir()), ""),
        ("bridge socket", br.socket_path(), ""),
        ("bridge log", br.bridge_log_path(), ""),
        ("action log", br.action_log_path(), ""),
        ("game dir", str(cfg.game_user_dir()), "launch's saves and autosaves"),
        ("game log", str(cfg.game_debug_log()), ""),
        ("server modules", str(server_dir()), ""),
    ]
    for k, v, src in rows:
        print(f"{k:<15} {v}" + (f"  ({src})" if src else ""))
    return 0


# ── parser ───────────────────────────────────────────────────────────────────

COMMANDS = [
    ("Get started", [("setup", "find CK3 and the bridge, write config and a play directory"),
                     ("doctor", "check each link in the chain; prints the next step")]),
    ("Play", [("launch", "start CK3 with the bridge loaded"),
              ("run", "unattended Claude Code session, recorded under runs/"),
              ("serve", "the MCP server (stdio for Claude Code, http for Claude Desktop)")]),
    ("Inspect", [("config", "each resolved setting and where it came from")]),
]
EPILOG = ("Exit codes: 0 ok, 1 failed, 2 usage error, 3 timed out waiting for the game (run: claude's code).\n"
          "`ck3agent COMMAND --help` lists a command's options.")


class _TopParser(argparse.ArgumentParser):
    def format_help(self) -> str:
        out = [f"usage: {self.prog} [-h] [-V] COMMAND ...", "", "Let Claude play Crusader Kings III.", ""]
        for title, cmds in COMMANDS:
            out += [f"{title}:"] + [f"  {n:<8}{h}" for n, h in cmds] + [""]
        out += ["Options:", "  -h, --help     show this help",
                "  -V, --version  version and the supported CK3 build", "", EPILOG]
        return "\n".join(out) + "\n"


class _Version(argparse.Action):
    """argparse's own version action wraps the line at the terminal width."""

    def __call__(self, parser, namespace, values, option_string=None):
        print(VERSION_LINE)
        parser.exit()


def _alias(p: argparse.ArgumentParser, old: str, dest: str) -> None:
    p.add_argument(old, dest=dest, action="store_true", help=argparse.SUPPRESS)


def build_parser() -> argparse.ArgumentParser:
    ap = _TopParser(prog="ck3agent")
    ap.add_argument("-V", "--version", action=_Version, nargs=0)
    sub = ap.add_subparsers(dest="cmd", metavar="COMMAND", parser_class=argparse.ArgumentParser)
    raw = argparse.RawDescriptionHelpFormatter

    game = argparse.ArgumentParser(add_help=False)
    game.add_argument("--ck3", metavar="PATH", help="the ck3 binary (default: CK3_BINARY, config, Steam)")
    game.add_argument("--bridge", metavar="PATH", help="libck3_bridge.so (default: CK3AGENT_BRIDGE, config, installed)")
    playp = argparse.ArgumentParser(add_help=False)
    playp.add_argument("--play-dir", metavar="DIR", help="Claude Code project dir (default: config, ~/ck3agent-play)")

    s = sub.add_parser("setup", parents=[game, playp], description="Find CK3 and the bridge, "
                       "write config, and set up a play directory. Exits 1 on an unsupported game build.")
    s.add_argument("--no-hook", action="store_true", help="skip the live-state UserPromptSubmit hook")
    s.add_argument("--no-allow", action="store_true",
                   help=f"skip the permissions.allow entries ({', '.join(ALLOW)})")
    s.add_argument("--force", action="store_true", help="replace CLAUDE.md and the ck3 entry in .mcp.json")
    s.set_defaults(fn=cmd_setup)

    s = sub.add_parser("doctor", parents=[game, playp], description="Check each link in the chain. "
                       "The last line names the next step.")
    s.set_defaults(fn=cmd_doctor)

    s = sub.add_parser("launch", parents=[game], formatter_class=raw, description=(
        "Start CK3 with the bridge loaded. Example: ck3agent launch --continue -- -debug_mode\n"
        "The game gets its own user-data directory (`ck3agent config` names it): its saves,\n"
        "autosaves and logs land there. Your own CK3 saves are read, never written."))
    s.add_argument("--continue", dest="cont", action="store_true",
                   help=f"load the save the launched game played last (the first time: a copy of the "
                        f"save your CK3 played last); waits {CONTINUE_WAIT}s ({FIRST_LOAD_WAIT}s on the first start) "
                        f"and dismisses the observe prompt")
    s.add_argument("--save", metavar="PATH",
                   help="copy this .ck3 into the game directory and load it (implies --continue)")
    s.add_argument("--wait", type=int, metavar="SECS",
                   help=f"wait up to SECS for the game: with --continue/--save until it is active, "
                        f"else until its main menu (default {CONTINUE_WAIT}, "
                        f"{FIRST_LOAD_WAIT} on the first start in the game directory)")
    s.add_argument("--no-wait", action="store_true", help="return as soon as CK3 starts")
    s.add_argument("--no-pause", action="store_true",
                   help="leave the clock running once the game is active (default: pause it)")
    s.add_argument("--native-wayland", action="store_true",
                   help="keep WAYLAND_DISPLAY (default under Wayland: SDL_VIDEODRIVER=x11)")
    s.add_argument("--allow-unsupported-build", dest="allow_unsupported", action="store_true",
                   help="launch on another build (CK3_STRICT_BUILD_ID=0); expect breakage")
    _alias(s, "--unsupported", "allow_unsupported")
    s.add_argument("game_args", nargs="*", metavar="-- GAME_ARGS", help="passed to the ck3 binary, after '--'")
    s.set_defaults(fn=cmd_launch)

    s = sub.add_parser("serve", description="Run the MCP server. Claude Code starts it via .mcp.json.")
    s.add_argument("--transport", choices=["stdio", "http"], default="stdio",
                   help="stdio for Claude Code (default), http for Claude Desktop")
    s.add_argument("--host", default="127.0.0.1", help="http bind address (loopback unless --allow-remote)")
    s.add_argument("--port", type=int, default=8000, help="http port (default 8000)")
    s.add_argument("--allow-remote", action="store_true", help="allow a non-loopback --host; the server has no auth")
    s.add_argument("-v", "--verbose", action="store_true", help="debug logging on stderr")
    s.set_defaults(fn=cmd_serve)

    s = sub.add_parser("run", parents=[playp], formatter_class=raw, description=(
        "Unattended Claude Code session, recorded under <play-dir>/runs/; pauses the game and "
        "releases the ruler at the end."))
    s.add_argument("--goal", help="goal text for the run prompt")
    s.add_argument("--prompt-file", metavar="PATH", help="use this file as the whole prompt")
    s.add_argument("--cycles", type=int, default=50, metavar="N",
                   help="stop the run after N cycles, counted by the run (default 50, 0 = no cycle stop)")
    s.add_argument("--max-turns", type=int, default=400, metavar="N", help="claude --max-turns (default 400, 0 = none)")
    s.add_argument("--model", help="claude --model")
    s.add_argument("--until-date", type=parse_date, metavar="Y.M.D", help="stop at this game date")
    s.add_argument("--wall-seconds", type=int, metavar="N", help="stop after N seconds of wall time")
    s.add_argument("--dangerously-skip-permissions", dest="skip_permissions", action="store_true",
                   help="no allow-list: Claude may run any tool")
    _alias(s, "--yolo", "skip_permissions")
    s.add_argument("--quiet", action="store_true", help="do not echo the session")
    s.add_argument("--no-game-check", action="store_true", help="start even if the bridge is not up")
    s.add_argument("claude_args", nargs="*", metavar="-- CLAUDE_ARGS", help="passed to claude, after '--'")
    s.set_defaults(fn=cmd_run)

    s = sub.add_parser("config", parents=[game, playp], description="Each resolved setting and its "
                       "source: flag, env, config, discovery or default.")
    s.set_defaults(fn=cmd_config)

    sub.add_parser("hook-state").set_defaults(fn=cmd_hook_state)
    sub.add_parser("version").set_defaults(fn=cmd_version)
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    a = ap.parse_args(argv)
    if not getattr(a, "fn", None):
        sys.stderr.write(ap.format_help())
        return 2
    try:
        return int(a.fn(a) or 0)
    except tomllib.TOMLDecodeError as e:
        print(f"{cfg.config_path()}: {e}. Fix or delete it, then rerun setup.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
