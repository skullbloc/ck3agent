"""ck3agent command line.

    ck3agent setup      find CK3 + the bridge, write config, create a play directory
    ck3agent doctor     check every link in the chain and say which one is broken
    ck3agent launch     start CK3 with the bridge loaded
    ck3agent serve      run the MCP server (stdio by default; what Claude Code spawns)
    ck3agent run        a scripted, unattended Claude Code session against the game
    ck3agent hook-state the Claude Code UserPromptSubmit hook (live game state)
    ck3agent config     show the active configuration
    ck3agent version
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import shutil
import socket
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

from . import __version__, bootstrap_server, server_dir
from . import _config as cfg
from ._bridge_meta import (SUPPORTED_BUILD_ID, SUPPORTED_GAME_VERSION, SUPPORTED_PLATFORM)
from ._elf import NotAnElf, read_build_id

PROTOCOL_DIR = Path(__file__).resolve().parent / "protocol"
DEFAULT_PLAY_DIR = Path.home() / "ck3agent-play"
MCP_SERVER_NAME = "ck3"


# ── helpers ──────────────────────────────────────────────────────────────────

def _resolve_binary(c: cfg.Config, override: str | None = None) -> Path | None:
    """Precedence: explicit flag > CK3_BINARY > config > Steam discovery."""
    for cand in (override, os.environ.get("CK3_BINARY"), c.ck3_binary or None):
        if cand:
            return Path(cand)
    return cfg.find_ck3_binary()


def _resolve_bridge(c: cfg.Config, override: str | None = None) -> Path | None:
    """Precedence: explicit flag > CK3AGENT_BRIDGE > config > installed location."""
    for cand in (override, os.environ.get("CK3AGENT_BRIDGE"), c.bridge_so or None):
        if cand:
            return Path(cand)
    return cfg.find_bridge_so()


def _socket_path() -> str:
    bootstrap_server()
    import instance  # noqa: WPS433  (server module, bare import by design)
    return instance.socket_path()


def _bridge_log_path() -> str:
    bootstrap_server()
    import instance  # noqa
    return instance.bridge_log()


def _socket_alive(path: str, timeout: float = 1.0) -> bool:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(path)
        return True
    except OSError:
        return False
    finally:
        s.close()


def _ok(label: str, detail: str = "") -> None:
    print(f"  [ ok ] {label}" + (f" — {detail}" if detail else ""))


def _warn(label: str, detail: str = "") -> None:
    print(f"  [warn] {label}" + (f" — {detail}" if detail else ""))


def _fail(label: str, detail: str = "") -> None:
    print(f"  [FAIL] {label}" + (f" — {detail}" if detail else ""))


def _mcp_json(play_dir: Path) -> dict:
    exe = shutil.which("ck3agent") or sys.argv[0]
    return {"mcpServers": {MCP_SERVER_NAME: {"command": exe, "args": ["serve"]}}}


# ── setup ────────────────────────────────────────────────────────────────────

def cmd_setup(a: argparse.Namespace) -> int:
    c = cfg.Config.load()
    print(f"ck3agent {__version__} setup — supports CK3 {SUPPORTED_GAME_VERSION} ({SUPPORTED_PLATFORM})")

    binary = _resolve_binary(c, a.ck3)
    if not binary or not binary.is_file():
        _fail("CK3 binary not found", "pass --ck3 /path/to/Crusader Kings III/binaries/ck3 or set CK3_BINARY")
        return 1
    _ok("CK3 binary", str(binary))
    _check_build_id(binary)

    bridge = _resolve_bridge(c, a.bridge)
    if not bridge or not bridge.is_file():
        _fail("bridge library not found", f"expected {cfg.data_dir()/'lib/libck3_bridge.so'} (run install.sh) or pass --bridge")
        return 1
    _ok("bridge library", str(bridge))

    csv = cfg.find_trigger_csv()
    if csv:
        _ok("trigger catalogue", str(csv))
    else:
        _warn("trigger catalogue missing", "discover(what=evaluate_trigger) will be unavailable")

    play = Path(a.play_dir).expanduser()
    play.mkdir(parents=True, exist_ok=True)
    mcp_path = play / ".mcp.json"
    if mcp_path.exists() and not a.force:
        _ok(".mcp.json kept", str(mcp_path))
    else:
        mcp_path.write_text(json.dumps(_mcp_json(play), indent=2) + "\n")
        _ok(".mcp.json written", str(mcp_path))
    claude_md = play / "CLAUDE.md"
    if claude_md.exists() and not a.force:
        _ok("CLAUDE.md kept", str(claude_md))
    else:
        shutil.copy(PROTOCOL_DIR / "CLAUDE.md", claude_md)
        _ok("CLAUDE.md written", "the play protocol Claude Code follows in this directory")
    if not a.no_hook:
        settings = play / ".claude" / "settings.json"
        settings.parent.mkdir(exist_ok=True)
        hook = {"hooks": {"UserPromptSubmit": [{"hooks": [{"type": "command",
                "command": f"{shutil.which('ck3agent') or 'ck3agent'} hook-state"}]}]}}
        if settings.exists() and not a.force:
            _ok(".claude/settings.json kept", str(settings))
        else:
            settings.write_text(json.dumps(hook, indent=2) + "\n")
            _ok("live-state hook installed", "each prompt gets a fresh game-state block")

    c.ck3_binary = str(binary)
    c.bridge_so = str(bridge)
    c.trigger_csv = str(csv) if csv else ""
    c.play_dir = str(play)
    p = c.save()
    _ok("config saved", str(p))
    print()
    print("Next:")
    print("  1. ck3agent launch --continue      # start CK3 with the bridge (or launch via Steam with LD_PRELOAD)")
    print("  2. load a save or start a game, then in another terminal:")
    print(f"  3. cd {play} && claude            # Claude Code picks up .mcp.json and CLAUDE.md")
    print("  (ck3agent doctor checks every step.)")
    return 0


def _check_build_id(binary: Path) -> bool:
    try:
        bid = read_build_id(binary)
    except (NotAnElf, OSError) as e:
        _fail("cannot read BuildID", str(e))
        return False
    ver = cfg.game_version_from_install(binary) or "unknown version"
    if bid == SUPPORTED_BUILD_ID:
        _ok("CK3 build supported", f"{ver}, BuildID {bid[:12]}…")
        return True
    _fail("CK3 build NOT supported by this release",
          f"installed {ver} (BuildID {bid}); this release supports CK3 {SUPPORTED_GAME_VERSION} "
          f"(BuildID {SUPPORTED_BUILD_ID}). The bridge will refuse to load. "
          "Check the releases page for a build matching your game version.")
    return False


# ── doctor ───────────────────────────────────────────────────────────────────

def cmd_doctor(a: argparse.Namespace) -> int:
    failed = 0
    print(f"ck3agent {__version__} doctor — supports CK3 {SUPPORTED_GAME_VERSION} ({SUPPORTED_PLATFORM})")
    if sys.version_info < (3, 11):
        _fail("python", f"{sys.version.split()[0]} (need 3.11+)"); failed += 1
    else:
        _ok("python", sys.version.split()[0])
    try:
        import mcp  # noqa
        _ok("mcp package", getattr(mcp, "__version__", "present"))
    except ImportError:
        _fail("mcp package missing", "pip install ck3agent (or mcp)"); failed += 1

    c = cfg.Config.load()
    if cfg.config_path().exists():
        _ok("config", str(cfg.config_path()))
    else:
        _warn("no config yet", "run `ck3agent setup`")

    binary = _resolve_binary(c)
    if binary and binary.is_file():
        _ok("CK3 binary", str(binary))
        if not _check_build_id(binary):
            failed += 1
    else:
        _fail("CK3 binary not found", "set CK3_BINARY or run setup --ck3"); failed += 1

    bridge = _resolve_bridge(c)
    if bridge and bridge.is_file():
        _ok("bridge library", str(bridge))
    else:
        _fail("bridge library not found", str(cfg.data_dir() / "lib/libck3_bridge.so")); failed += 1

    if shutil.which("claude"):
        _ok("claude CLI", shutil.which("claude"))
    else:
        _warn("claude CLI not on PATH", "needed for `claude` play sessions and `ck3agent run`")

    play = Path(c.play_dir) if c.play_dir else DEFAULT_PLAY_DIR
    if (play / ".mcp.json").exists():
        _ok("play directory", str(play))
    else:
        _warn("play directory not set up", f"{play} has no .mcp.json; run `ck3agent setup`")

    sock = _socket_path()
    if _socket_alive(sock):
        _ok("bridge socket", sock)
        failed += 0 if _handshake_report(sock) else 1
    else:
        _warn("bridge socket not listening", f"{sock} — CK3 not running with the bridge? (`ck3agent launch`)")
        failed += _bridge_log_report()
    print()
    print("all checks passed" if not failed else f"{failed} check(s) failed")
    return 1 if failed else 0


def _handshake_report(sock: str) -> bool:
    bootstrap_server()
    try:
        from bridge_client import BridgeClient, HandshakeRefused  # noqa
        bc = BridgeClient(sock)
        if not bc.connect(timeout=3.0):
            _fail("bridge connect", "socket exists but refused the connection"); return False
        try:
            ack = bc.handshake(agent_version=__version__, protocol_version=2,
                               client_name="ck3agent-doctor", mode="observe", timeout=8.0)
        finally:
            bc.close()
    except HandshakeRefused as e:
        _fail("bridge handshake refused", str(e)); return False
    except Exception as e:  # noqa: BLE001
        _fail("bridge handshake", repr(e)); return False
    if not ack:
        _fail("bridge handshake", "no ack within 8s"); return False
    seen = ack.get("observed_build_id") or ack.get("build_id") or "?"
    state = ack.get("session_state") or ack.get("status") or "?"
    _ok("bridge handshake", f"session_state={state}, observed BuildID {str(seen)[:12]}…")
    return True


def _bridge_log_report() -> int:
    log = Path(_bridge_log_path())
    if not log.exists():
        return 0
    try:
        tail = log.read_text(errors="replace").splitlines()[-200:]
    except OSError:
        return 0
    hits = [i for i, l in enumerate(tail) if "REFUSED" in l]
    if hits:
        _fail("bridge refused to load (from the bridge log)", log.as_posix())
        for l in tail[hits[-1]:hits[-1] + 5]:
            print("         " + l.strip())
        return 1
    _ok("bridge log", f"{log} (no refusal recorded)")
    return 0


# ── launch ───────────────────────────────────────────────────────────────────

def cmd_launch(a: argparse.Namespace) -> int:
    c = cfg.Config.load()
    binary = _resolve_binary(c)
    bridge = _resolve_bridge(c)
    if not binary or not binary.is_file() or not bridge or not bridge.is_file():
        _fail("not configured", "run `ck3agent setup` first"); return 1
    if not _check_build_id(binary) and not a.unsupported:
        print("refusing to launch an unsupported build (pass --unsupported to try anyway)")
        return 1
    sock = _socket_path()
    if _socket_alive(sock):
        _fail("a bridge is already listening", f"{sock} — another CK3 with the bridge is running"); return 1

    env = dict(os.environ)
    env["LD_PRELOAD"] = str(bridge) + (":" + env["LD_PRELOAD"] if env.get("LD_PRELOAD") else "")
    csv = Path(c.trigger_csv) if c.trigger_csv else cfg.find_trigger_csv()
    if csv:
        env.setdefault("CK3_TRIGGER_CATALOGUE_CSV", str(csv))
    if a.unsupported:
        env["CK3_STRICT_BUILD_ID"] = "0"
    argv = [str(binary)] + (["--continuelastsave"] if a.cont else []) + list(a.game_args)
    logs = cfg.data_dir() / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    out = logs / f"ck3-{datetime.now():%Y%m%d-%H%M%S}.log"
    with out.open("wb") as fh:
        proc = subprocess.Popen(argv, cwd=binary.parent, env=env, stdout=fh, stderr=subprocess.STDOUT,
                                start_new_session=True)
    print(f"launched CK3 pid {proc.pid}; game output → {out}")
    print(f"bridge log → {_bridge_log_path()}")
    if a.wait:
        deadline = time.time() + a.wait
        up = False
        while time.time() < deadline:
            if proc.poll() is not None:
                _fail("CK3 exited early", f"code {proc.returncode}; see {out}"); return 1
            if _socket_alive(sock):
                _ok("bridge socket up", sock); up = True; break
            time.sleep(1.0)
        if not up:
            _warn("bridge socket not up yet", f"after {a.wait}s; the bridge opens it once the binary is verified. `ck3agent doctor` to check.")
            return _bridge_log_report()
        if a.cont:
            return _wait_for_active(sock, deadline, proc)
    return 0


def _wait_for_active(sock: str, deadline: float, proc: "subprocess.Popen") -> int:
    """After --continue: CK3 lands on a confirmation screen (an Observe button for
    observer saves). Poll the bridge; once the lobby view exists, ask it to enter
    observe mode, which is what dismisses the prompt. A normal player save needs a
    click on Continue in the game window instead; we say so and return."""
    bootstrap_server()
    from bridge_client import BridgeClient  # noqa
    last_reason = ""
    while time.time() < deadline:
        if proc.poll() is not None:
            _fail("CK3 exited", f"code {proc.returncode}"); return 1
        try:
            bc = BridgeClient(sock)
            if bc.connect(timeout=3.0):
                try:
                    bc.handshake(agent_version=__version__, client_name="ck3agent-launch", timeout=8.0)
                    bc.send({"actions": [{"action": "get_tick_info", "params": {}, "request_id": "launch-tick"}]})
                    r = bc.recv(6.0) or {}
                    if r.get("session_state") == "active":
                        _ok("game active", f"date {r.get('game_date')}"); return 0
                    bc.send({"actions": [{"action": "enter_observe_mode", "params": {}, "request_id": "launch-observe"}]})
                    for _ in range(3):
                        r = bc.recv(10.0)
                        if r and r.get("action") == "enter_observe_mode":
                            last_reason = str(r.get("reason") or "")
                            break
                finally:
                    bc.close()
        except Exception as e:  # noqa: BLE001
            last_reason = repr(e)
        time.sleep(5.0)
    _warn("game not active yet", f"last bridge reason: {last_reason or 'none'}. If this is a player save, click Continue in the game; `ck3agent doctor` reports the state.")
    return 0


# ── serve ────────────────────────────────────────────────────────────────────

def cmd_serve(a: argparse.Namespace) -> int:
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s", stream=sys.stderr)
    bootstrap_server()
    import mcp_server  # noqa
    if a.transport == "http":
        mcp_server.mcp.settings.host = a.host
        mcp_server.mcp.settings.port = a.port
        print(f"ck3agent MCP server on http://{a.host}:{a.port}/mcp (streamable-http)", file=sys.stderr)
        mcp_server.mcp.run(transport="streamable-http")
    else:
        mcp_server.mcp.run(transport="stdio")
    return 0


# ── run (scripted Claude Code session) ───────────────────────────────────────

def cmd_run(a: argparse.Namespace) -> int:
    claude = shutil.which("claude")
    if not claude:
        _fail("claude CLI not on PATH", "install Claude Code: https://docs.claude.com/en/docs/claude-code"); return 1
    c = cfg.Config.load()
    play = Path(a.play_dir).expanduser() if a.play_dir else (Path(c.play_dir) if c.play_dir else DEFAULT_PLAY_DIR)
    if not (play / ".mcp.json").exists():
        _fail("play directory not set up", f"{play}: run `ck3agent setup`"); return 1
    sock = _socket_path()
    if not _socket_alive(sock) and not a.no_game_check:
        _fail("bridge socket not listening", f"{sock}: start the game first (`ck3agent launch`)"); return 1

    if a.prompt_file:
        prompt = Path(a.prompt_file).read_text(encoding="utf-8")
    else:
        prompt = (PROTOCOL_DIR / "run-prompt.md").read_text(encoding="utf-8")
    prompt = prompt.replace("{{goal}}", a.goal or "Play well: survive, grow the realm, keep the succession intact.")
    prompt = prompt.replace("{{cycles}}", str(a.cycles))

    runs = play / "runs"
    runs.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    (runs / f"{stamp}.prompt.md").write_text(prompt, encoding="utf-8")
    log = runs / f"{stamp}.log"

    argv = [claude, "-p", prompt, "--mcp-config", str(play / ".mcp.json")]
    if a.model:
        argv += ["--model", a.model]
    if a.max_turns:
        argv += ["--max-turns", str(a.max_turns)]
    if a.yolo:
        argv += ["--dangerously-skip-permissions"]
    else:
        argv += ["--allowedTools", f"mcp__{MCP_SERVER_NAME}", "Bash(sleep:*)"]
    argv += a.claude_args
    print(f"run {stamp}: {len(prompt)} char prompt, {a.cycles} cycles; log → {log}")
    with log.open("w", encoding="utf-8") as fh:
        proc = subprocess.Popen(argv, cwd=play, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        assert proc.stdout
        for line in proc.stdout:
            fh.write(line)
            if not a.quiet:
                sys.stdout.write(line)
        rc = proc.wait()
    print(f"run {stamp} finished with code {rc}")
    return rc


# ── hook-state ───────────────────────────────────────────────────────────────

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


# ── config / version ─────────────────────────────────────────────────────────

def cmd_config(a: argparse.Namespace) -> int:
    p = cfg.config_path()
    print(f"# {p}{'' if p.exists() else ' (not written yet)'}")
    if p.exists():
        print(p.read_text(), end="")
    print(f"# data dir: {cfg.data_dir()}")
    print(f"# bridge socket: {_socket_path()}")
    print(f"# server modules: {server_dir()}")
    return 0


def cmd_version(a: argparse.Namespace) -> int:
    print(f"ck3agent {__version__} (CK3 {SUPPORTED_GAME_VERSION} {SUPPORTED_PLATFORM}, BuildID {SUPPORTED_BUILD_ID})")
    return 0


# ── parser ───────────────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="ck3agent", description="Let Claude play Crusader Kings III.")
    ap.add_argument("--version", action="version", version=f"ck3agent {__version__}")
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("setup", help="find CK3 and the bridge, write config, create a play directory")
    s.add_argument("--ck3", help="path to the ck3 binary (default: Steam discovery / CK3_BINARY)")
    s.add_argument("--bridge", help="path to libck3_bridge.so (default: the installed one)")
    s.add_argument("--play-dir", default=str(DEFAULT_PLAY_DIR), help=f"Claude Code project dir (default {DEFAULT_PLAY_DIR})")
    s.add_argument("--no-hook", action="store_true", help="do not install the live-state UserPromptSubmit hook")
    s.add_argument("--force", action="store_true", help="overwrite existing play-dir files")
    s.set_defaults(fn=cmd_setup)

    s = sub.add_parser("doctor", help="check the whole chain and report what is broken")
    s.set_defaults(fn=cmd_doctor)

    s = sub.add_parser("launch", help="start CK3 with the bridge loaded")
    s.add_argument("--continue", dest="cont", action="store_true", help="pass --continuelastsave to CK3")
    s.add_argument("--wait", type=int, default=0, metavar="SECS", help="wait up to SECS for the bridge socket")
    s.add_argument("--unsupported", action="store_true", help="launch even on an unsupported build (CK3_STRICT_BUILD_ID=0)")
    s.add_argument("game_args", nargs="*", help="extra arguments for the ck3 binary")
    s.set_defaults(fn=cmd_launch)

    s = sub.add_parser("serve", help="run the MCP server")
    s.add_argument("--transport", choices=["stdio", "http"], default="stdio")
    s.add_argument("--host", default="127.0.0.1")
    s.add_argument("--port", type=int, default=8000)
    s.add_argument("-v", "--verbose", action="store_true")
    s.set_defaults(fn=cmd_serve)

    s = sub.add_parser("run", help="scripted, unattended Claude Code session against the running game")
    s.add_argument("--goal", help="one-paragraph goal substituted into the run prompt")
    s.add_argument("--prompt-file", help="use this file as the whole starting prompt instead")
    s.add_argument("--cycles", type=int, default=50, help="how many observe/act cycles the prompt asks for")
    s.add_argument("--max-turns", type=int, default=0, help="hard cap on agentic turns (claude --max-turns)")
    s.add_argument("--model", help="model name passed to claude --model")
    s.add_argument("--play-dir", help="play directory (default: from config)")
    s.add_argument("--yolo", action="store_true", help="--dangerously-skip-permissions instead of an allow-list")
    s.add_argument("--quiet", action="store_true", help="do not echo the session to stdout")
    s.add_argument("--no-game-check", action="store_true")
    s.add_argument("claude_args", nargs="*", help="extra arguments passed through to claude (after --)")
    s.set_defaults(fn=cmd_run)

    s = sub.add_parser("hook-state", help="Claude Code UserPromptSubmit hook: print a live game-state block")
    s.set_defaults(fn=cmd_hook_state)

    s = sub.add_parser("config", help="show the active configuration")
    s.set_defaults(fn=cmd_config)

    s = sub.add_parser("version", help="print version and supported CK3 build")
    s.set_defaults(fn=cmd_version)
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    a = ap.parse_args(argv)
    return int(a.fn(a) or 0)


if __name__ == "__main__":
    sys.exit(main())
