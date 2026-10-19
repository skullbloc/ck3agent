"""CLI tests. No game, no bridge binary; conftest.py makes HOME and XDG private."""
from __future__ import annotations

import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

from ck3agent import __version__, cli, _bridge as br, _config as cfg
from ck3agent._bridge_meta import SUPPORTED_BUILD_ID, SUPPORTED_GAME_VERSION
from ck3agent._elf import NotAnElf, read_build_id

ROOT = Path(__file__).resolve().parents[1]
PY = Path(sys.executable).resolve()


def py_build_id() -> str:
    bid = read_build_id(PY)
    if not bid:
        pytest.skip("python binary carries no build-id")
    return bid


@pytest.fixture
def install(hermetic, monkeypatch):
    """A fake install: the python binary stands in for ck3, a stub file for the .so."""
    ck3 = hermetic / "steam/steamapps/common/Crusader Kings III/binaries/ck3"
    ck3.parent.mkdir(parents=True)
    shutil.copy(PY, ck3)
    so = hermetic / ".local/share/ck3agent/lib/libck3_bridge.so"
    so.parent.mkdir(parents=True)
    so.write_bytes(b"\x7fELF")
    return ck3, so


@pytest.fixture
def supported(monkeypatch):
    monkeypatch.setattr(cli, "SUPPORTED_BUILD_ID", py_build_id())


# ── version, help ────────────────────────────────────────────────────────────

def test_version_matches_stamped_files():
    assert (ROOT / "VERSION").read_text().strip() == __version__
    assert len(SUPPORTED_BUILD_ID) == 40 and int(SUPPORTED_BUILD_ID, 16)
    assert SUPPORTED_GAME_VERSION[0].isdigit()


def test_help_groups_and_hides_internal_commands(capsys):
    with pytest.raises(SystemExit) as e:
        cli.main(["--help"])
    assert e.value.code == 0
    out = capsys.readouterr().out
    for head in ("Get started:", "Play:", "Inspect:", "Exit codes:"):
        assert head in out
    assert "hook-state" not in out and "\n  version" not in out


def test_dash_V_prints_the_full_line(capsys):
    with pytest.raises(SystemExit):
        cli.main(["-V"])
    assert capsys.readouterr().out.strip() == cli.VERSION_LINE
    assert cli.main(["version"]) == 0


def test_python_dash_m_runs_the_cli():
    r = subprocess.run([sys.executable, "-m", "ck3agent", "--version"], cwd=ROOT,
                       capture_output=True, text=True)
    assert r.returncode == 0 and __version__ in r.stdout


def test_no_command_is_a_usage_error(capsys):
    assert cli.main([]) == 2
    assert "Get started:" in capsys.readouterr().err


# ── _elf ─────────────────────────────────────────────────────────────────────

def test_read_build_id_matches_readelf():
    try:
        out = subprocess.run(["readelf", "-n", str(PY)], capture_output=True, text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        pytest.skip("readelf unavailable")
    m = re.search(r"Build ID:\s*([0-9a-f]+)", out)
    if not m:
        pytest.skip("python binary carries no build-id")
    assert read_build_id(PY) == m.group(1)


@pytest.mark.parametrize("size", [3, 40, 2000])
def test_truncated_elf_raises_not_an_elf(tmp_path, size):
    p = tmp_path / "cut"
    p.write_bytes(PY.read_bytes()[:size])
    with pytest.raises(NotAnElf):
        read_build_id(p)


def test_stripped_section_headers_fall_back_to_pt_note(tmp_path):
    bid = py_build_id()
    data = bytearray(PY.read_bytes())
    struct.pack_into("<Q", data, 0x28, 0)          # e_shoff
    struct.pack_into("<H", data, 0x3C, 0)          # e_shnum
    p = tmp_path / "stripped"
    p.write_bytes(bytes(data))
    assert read_build_id(p) == bid


# ── _config ──────────────────────────────────────────────────────────────────

def test_empty_xdg_vars_count_as_unset(hermetic, monkeypatch):
    monkeypatch.setenv("XDG_CONFIG_HOME", "")
    monkeypatch.setenv("XDG_DATA_HOME", "")
    monkeypatch.setenv("CK3AGENT_CONFIG", "")
    assert cfg.config_dir() == hermetic / ".config/ck3agent"
    assert cfg.data_dir() == hermetic / ".local/share/ck3agent"
    assert cfg.config_path() == hermetic / ".config/ck3agent/config.toml"


def test_config_round_trip_keeps_extras_typed():
    extra = {"n": 3, "on": True, "ratio": 1.5, "items": [1, "a"], "weird key": "x",
             "text": 'q"\\ \n\t\x01\x7f', "table": {"k": 1, "s": "v"}}
    cfg.Config(ck3_binary="/a b/ck3", extra=extra).save()
    c = cfg.Config.load()
    assert c.ck3_binary == "/a b/ck3"
    assert c.extra == extra


def test_resolve_precedence_and_config_shows_sources(install, monkeypatch, capsys):
    ck3, so = install
    other = ck3.with_name("ck3-env")
    assert cfg.resolve("ck3_binary").source == "unset"
    cfg.Config(ck3_binary=str(ck3)).save()
    assert cfg.resolve("ck3_binary") == (ck3, "config")
    monkeypatch.setenv("CK3_BINARY", str(other))
    assert cfg.resolve("ck3_binary") == (other, "env CK3_BINARY")
    assert cfg.resolve("ck3_binary", flag="/x/ck3") == (Path("/x/ck3"), "flag")
    assert cfg.resolve("bridge_so") == (so, "discovery")
    assert cfg.resolve("play_dir").source == "default"
    assert cli.main(["config"]) == 0
    out = capsys.readouterr().out
    assert "(env CK3_BINARY)" in out and "(discovery)" in out and "(default)" in out


def test_trigger_csv_honours_env_then_config(tmp_path, monkeypatch):
    a, b = tmp_path / "a.csv", tmp_path / "b.csv"
    a.write_text("x"), b.write_text("y")
    assert cfg.find_trigger_csv(cfg.Config(trigger_csv=str(a))) == a
    monkeypatch.setenv("CK3_TRIGGER_CATALOGUE_CSV", str(b))
    assert cfg.find_trigger_csv(cfg.Config(trigger_csv=str(a))) == b


# ── setup ────────────────────────────────────────────────────────────────────

def test_setup_exits_1_on_unsupported_build_and_writes_nothing(install, tmp_path, capsys):
    ck3, _ = install
    play = tmp_path / "play"
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 1
    out = capsys.readouterr().out
    assert f"needs CK3 {SUPPORTED_GAME_VERSION}" in out and cli.RELEASES_URL in out
    assert "Next:" not in out
    assert not (play / ".mcp.json").exists() and not cfg.config_path().exists()


def test_setup_writes_play_dir_and_config(install, supported, tmp_path, capsys):
    ck3, so = install
    play = tmp_path / "play"
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 0
    out = capsys.readouterr().out
    argv = cli.exe_argv()
    mcp = json.loads((play / ".mcp.json").read_text())["mcpServers"]["ck3"]
    assert [mcp["command"], *mcp["args"]] == argv + ["serve"]
    settings = json.loads((play / ".claude/settings.json").read_text())
    hook = settings["hooks"]["UserPromptSubmit"][0]["hooks"][0]["command"]
    assert hook.split() == argv + ["hook-state"]
    assert settings["permissions"]["allow"] == cli.ALLOW
    assert (play / "CLAUDE.md").exists()
    assert f"LD_PRELOAD={so.resolve()} %command%" in out
    c = cfg.Config.load()
    assert (c.ck3_binary, c.bridge_so, c.play_dir) == (str(ck3), str(so.resolve()), str(play.resolve()))


def test_setup_merges_existing_settings(install, supported, tmp_path, monkeypatch):
    ck3, _ = install
    play = tmp_path / "play"
    (play / ".claude").mkdir(parents=True)
    user = {"model": "x", "permissions": {"allow": ["Bash(ls:*)"], "deny": ["Read(.env)"]},
            "hooks": {"UserPromptSubmit": [{"hooks": [{"type": "command", "command": "my-hook"}]}],
                      "Stop": [{"hooks": [{"type": "command", "command": "bell"}]}]}}
    (play / ".claude/settings.json").write_text(json.dumps(user))
    monkeypatch.setattr(cli, "exe_argv", lambda: ["/opt/my tools/ck3agent"])
    for _ in range(2):                                     # idempotent
        assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 0
    s = json.loads((play / ".claude/settings.json").read_text())
    assert s["model"] == "x" and s["permissions"]["deny"] == ["Read(.env)"]
    assert s["permissions"]["allow"] == ["Bash(ls:*)", *cli.ALLOW]
    assert s["hooks"]["Stop"] == user["hooks"]["Stop"]
    cmds = [h["command"] for g in s["hooks"]["UserPromptSubmit"] for h in g["hooks"]]
    assert cmds == ["my-hook", "'/opt/my tools/ck3agent' hook-state"]


def test_setup_no_allow_and_bad_json(install, supported, tmp_path):
    ck3, _ = install
    play = tmp_path / "play"
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play), "--no-allow"]) == 0
    assert "permissions" not in json.loads((play / ".claude/settings.json").read_text())
    (play / ".claude/settings.json").write_text("{not json")
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 1
    assert (play / ".claude/settings.json").read_text() == "{not json"


def test_exe_argv_falls_back_to_python_dash_m(tmp_path, monkeypatch):
    fake = tmp_path / "bin/python"
    fake.parent.mkdir()
    monkeypatch.setattr(sys, "executable", str(fake))
    assert cli.exe_argv() == [str(fake), "-m", "ck3agent"]
    (fake.parent / "ck3agent").write_text("#!/bin/sh\n")
    (fake.parent / "ck3agent").chmod(0o755)
    assert cli.exe_argv() == [str(fake.parent / "ck3agent")]


def _fake_venv(d: Path) -> Path:
    (d / "bin").mkdir(parents=True)
    (d / "bin/python").write_text("")
    (d / "bin/ck3agent").write_text("#!/bin/sh\n")
    (d / "bin/ck3agent").chmod(0o755)
    return d


def test_exe_argv_goes_through_the_stable_venv_link(monkeypatch):
    """install.sh: DATA/venv -> DATA/venv.<ver>.<random>, older ones deleted on the next install."""
    data = cfg.data_dir()
    real = _fake_venv(data / "venv.2026.10.0.123")
    (data / "venv").symlink_to(real.name)
    monkeypatch.setattr(sys, "executable", str(real / "bin/python"))
    monkeypatch.setattr(sys, "prefix", str(real))
    assert cli.exe_argv() == [str(data / "venv/bin/ck3agent")]
    # a venv the link does not point at (pipx, a dev checkout) keeps its own path
    other = _fake_venv(data.parent / "elsewhere")
    monkeypatch.setattr(sys, "executable", str(other / "bin/python"))
    monkeypatch.setattr(sys, "prefix", str(other))
    assert cli.exe_argv() == [str(other / "bin/ck3agent")]


def test_setup_replaces_a_ck3_entry_whose_command_is_gone(install, supported, tmp_path, capsys):
    ck3, _ = install
    play = tmp_path / "play"
    play.mkdir()
    gone = {"command": str(tmp_path / "venv.old.1/bin/ck3agent"), "args": ["serve"]}
    mine = {"command": sys.executable, "args": ["-m", "ck3agent", "serve"], "env": {"X": "1"}}
    for entry, kept in ((mine, True), (gone, False)):
        (play / ".mcp.json").write_text(json.dumps({"mcpServers": {"ck3": entry, "other": {"command": "x"}}}))
        assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 0
        d = json.loads((play / ".mcp.json").read_text())["mcpServers"]
        assert d["other"] == {"command": "x"}
        assert (d["ck3"] == entry) is kept
        assert cli.command_exists(d["ck3"]["command"])
    assert not cli.command_exists("no-such-ck3agent-cmd") and not cli.command_exists(None)


def test_doctor_fails_when_the_mcp_command_is_gone(install, supported, tmp_path, capsys):
    ck3, _ = install
    play = tmp_path / "play"
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 0
    d = json.loads((play / ".mcp.json").read_text())
    d["mcpServers"]["ck3"]["command"] = str(tmp_path / "venv.2026.10.0.99/bin/ck3agent")
    (play / ".mcp.json").write_text(json.dumps(d))
    capsys.readouterr()
    assert cli.main(["doctor"]) == 1
    out = capsys.readouterr().out
    assert "ck3 MCP command missing" in out and "venv.2026.10.0.99" in out
    assert "Next: ck3agent setup" in out


def test_setup_refuses_a_bridge_path_ld_preload_cannot_hold(install, supported, tmp_path):
    ck3, so = install
    bad = tmp_path / "with space" / "libck3_bridge.so"
    bad.parent.mkdir()
    shutil.copy(so, bad)
    assert cli.main(["setup", "--ck3", str(ck3), "--bridge", str(bad), "--play-dir", str(tmp_path / "p")]) == 1
    assert not cli.preload_ok(Path("/a:b.so")) and cli.preload_ok(Path("/a/b.so"))


# ── doctor ───────────────────────────────────────────────────────────────────

def test_doctor_unsupported_build_and_no_game(install, monkeypatch, capsys):
    monkeypatch.setenv("CK3_BINARY", str(install[0]))
    assert cli.main(["doctor"]) == 1
    out = capsys.readouterr().out
    assert "CK3 build not supported" in out and "bridge socket not listening" in out
    last = out.strip().splitlines()
    assert any(l.startswith("1 check(s) failed. Next: Your game: CK3") for l in last)
    assert "Traceback" not in out


def test_doctor_ready_without_a_game(install, supported, tmp_path, capsys):
    ck3, _ = install
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(tmp_path / "play")]) == 0
    capsys.readouterr()
    assert cli.main(["doctor"]) == 0
    assert capsys.readouterr().out.strip().endswith("Ready. Next: ck3agent launch --continue")


def test_doctor_matches_only_the_exact_refusal(tmp_path, capsys):
    log = Path(os.environ["CK3_BRIDGE_LOG"])
    log.write_text("[CK3-Bridge] deferred_drain: a deferred call was REFUSED and did not run\n")
    cli.main(["doctor"])
    assert "bridge refused to load" not in capsys.readouterr().out
    log.write_text("[CK3-Bridge] REFUSED: unsupported CK3 build.\n[CK3-Bridge]   BuildID deadbeef\n")
    cli.main(["doctor"])
    out = capsys.readouterr().out
    assert "bridge refused to load" in out and "deadbeef" in out


def test_doctor_ignores_a_refusal_older_than_the_last_launch(tmp_path, capsys):
    log = Path(os.environ["CK3_BRIDGE_LOG"])
    log.write_text("[CK3-Bridge] REFUSED: unsupported CK3 build.\n")
    old = time.time() - 3600
    os.utime(log, (old, old))
    logs = cfg.data_dir() / "logs"
    logs.mkdir(parents=True)
    (logs / f"ck3-{time.strftime('%Y%m%d-%H%M%S')}.log").write_text("")
    cli.main(["doctor"])
    assert "bridge refused to load" not in capsys.readouterr().out


# ── launch ───────────────────────────────────────────────────────────────────

def test_launch_env_switches_wayland_to_x11():
    so = Path("/lib/b.so")
    env, notes = cli.launch_env({"WAYLAND_DISPLAY": "wayland-0", "LD_PRELOAD": "/x.so /lib/b.so"},
                                so, None, False, False)
    assert env["SDL_VIDEODRIVER"] == "x11" and "WAYLAND_DISPLAY" not in env and notes
    assert env["LD_PRELOAD"] == "/lib/b.so:/x.so"
    env, _ = cli.launch_env({"WAYLAND_DISPLAY": "w"}, so, None, False, True)
    assert env["WAYLAND_DISPLAY"] == "w" and "SDL_VIDEODRIVER" not in env
    env, _ = cli.launch_env({"WAYLAND_DISPLAY": "w", "SDL_VIDEODRIVER": "wayland"}, so, None, True, False)
    assert env["SDL_VIDEODRIVER"] == "wayland" and env["CK3_STRICT_BUILD_ID"] == "0"


def test_launch_flags():
    p = cli.build_parser()
    a = p.parse_args(["launch", "--continue", "--unsupported", "--", "-debug_mode"])
    assert a.cont and a.allow_unsupported and a.game_args == ["-debug_mode"] and a.wait is None
    assert p.parse_args(["launch", "--allow-unsupported-build"]).allow_unsupported


def test_stage_save_repoints_continue_game(tmp_path):
    src = tmp_path / "my world.ck3"
    src.write_bytes(b"SAV")
    ptr = cfg.game_user_dir() / "continue_game.json"
    ptr.parent.mkdir(parents=True)
    ptr.write_text(json.dumps({"title": "autosave", "desc": "d"}))
    dst, digest = cli.stage_save(src)
    assert dst == cfg.game_user_dir() / "save games/my world.ck3" and dst.read_bytes() == b"SAV"
    assert json.loads(ptr.read_text()) == {"title": "my world", "desc": "d"}
    assert json.loads(ptr.with_name("continue_game.json.bak-ck3agent").read_text())["title"] == "autosave"
    assert digest == cli.sha256(src)
    with pytest.raises(ValueError):
        cli.stage_save(tmp_path / "nope.txt")


def test_wait_for_active_times_out_with_3(monkeypatch):
    class Proc:
        def poll(self):
            return None

    def unreachable(*a, **k):
        raise OSError("down")

    monkeypatch.setattr(br, "session", unreachable)
    monkeypatch.setattr(br.time, "sleep", lambda s: None)
    assert br.wait_for_active("/nope", time.time() + 0.05, Proc()) == 3


# ── serve ────────────────────────────────────────────────────────────────────

def test_serve_refuses_a_remote_host(capsys):
    assert cli.main(["serve", "--transport", "http", "--host", "0.0.0.0"]) == 2
    assert "--allow-remote" in capsys.readouterr().err


# ── run ──────────────────────────────────────────────────────────────────────

def test_run_argv(tmp_path):
    p = cli.build_parser()
    a = p.parse_args(["run", "--model", "m", "--", "--foo"])
    assert a.max_turns == 400 and not a.skip_permissions
    argv = cli.build_claude_argv("claude", "PROMPT", tmp_path / "m.json", a, strict=True)
    assert argv[:3] == ["claude", "-p", "PROMPT"]
    joined = " ".join(argv)
    assert "--output-format stream-json --verbose" in joined and "--strict-mcp-config" in argv
    assert "--max-turns 400" in joined and "--model m" in joined
    assert argv[argv.index("--allowedTools") + 1:] == [*cli.ALLOW, "--foo"]
    a = p.parse_args(["run", "--yolo", "--max-turns", "0"])
    argv = cli.build_claude_argv("claude", "P", tmp_path / "m.json", a, strict=False)
    assert "--dangerously-skip-permissions" in argv and "--allowedTools" not in argv
    assert "--max-turns" not in argv and "--strict-mcp-config" not in argv


def test_run_rejects_a_bad_until_date():
    with pytest.raises(SystemExit):
        cli.build_parser().parse_args(["run", "--until-date", "867-1-1"])
    assert cli.build_parser().parse_args(["run", "--until-date", "867.1.1"]).until_date == (867, 1, 1)


FAKE_CLAUDE = r'''#!@PY@
"""Stands in for `claude -p`. FAKE_MODE: finish | noresult | hang-result | hang-ignore.

With FAKE_SERVE set it starts the MCP server from --mcp-config, as Claude Code
does, and never writes the tool surface itself. It stops the server the way
Claude Code 2.1.302 does: SIGINT, SIGTERM 100 ms later, SIGKILL 400 ms after
that. stdin stays open. The server's exit status goes to $FAKE_ARGV.server_rc.
FAKE_WAIT_LOG: wait for this text in the server's stderr before the session
starts (default: the tool surface file, then FAKE_SETTLE seconds, default 1).
FAKE_MCP_CALL: a {"name", "arguments"} tools/call sent to the server over MCP
stdio once it is up; the reply goes to $FAKE_ARGV.mcp.
"""
import json, os, select, signal, subprocess, sys, time
args = sys.argv[1:]
if args == ["--help"]:
    print("  --strict-mcp-config  only these servers"); sys.exit(0)
if args == ["--version"]:
    print("9.9.9 (Claude Code)"); sys.exit(0)
mode = os.environ.get("FAKE_MODE", "finish")
open(os.environ["FAKE_ARGV"] + ".pid", "w").write(str(os.getpid()))
open(os.environ["FAKE_ARGV"], "w").write(json.dumps(args))
if os.environ.get("FAKE_ACTION_LOG"):
    open(os.environ["FAKE_ACTION_LOG"], "a").write('{"action":"during"}\n')
cfg = json.load(open(args[args.index("--mcp-config") + 1]))["mcpServers"]["ck3"]
server = None
if os.environ.get("FAKE_SERVE"):
    log = os.environ["FAKE_SERVE"]
    call = os.environ.get("FAKE_MCP_CALL")
    server = subprocess.Popen([cfg["command"], *cfg["args"]], env={**os.environ, **cfg.get("env", {})},
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE if call else subprocess.DEVNULL,
                              stderr=open(log, "w"))
    want = os.environ.get("FAKE_WAIT_LOG")
    for _ in range(600):
        if want and want in open(log, errors="replace").read():
            break
        if not want and os.path.exists(cfg["env"]["CK3_TOOL_SURFACE_OUT"]):
            break
        time.sleep(0.1)
    if not want:
        time.sleep(float(os.environ.get("FAKE_SETTLE", "1.0")))
    if call:
        def rpc(msg, want_id=None, deadline=60.0):
            server.stdin.write((json.dumps({"jsonrpc": "2.0", **msg}) + "\n").encode()); server.stdin.flush()
            end = time.monotonic() + deadline
            while want_id is not None and time.monotonic() < end:
                if select.select([server.stdout], [], [], 0.5)[0]:
                    line = server.stdout.readline()
                    if not line:
                        break
                    rep = json.loads(line)
                    if rep.get("id") == want_id:
                        return rep
            return None
        rpc({"id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                                                          "clientInfo": {"name": "fake", "version": "1"}}}, 1)
        rpc({"method": "notifications/initialized"})
        rep = rpc({"id": 2, "method": "tools/call", "params": json.loads(call)}, 2)
        open(os.environ["FAKE_ARGV"] + ".mcp", "w").write(json.dumps(rep))


def stop_server():
    if not server:
        return
    if server.poll() is None:
        for sig, grace in ((signal.SIGINT, 0.1), (signal.SIGTERM, 0.4), (signal.SIGKILL, None)):
            server.send_signal(sig)
            try:
                server.wait(grace)
                break
            except subprocess.TimeoutExpired:
                pass
    open(os.environ["FAKE_ARGV"] + ".server_rc", "w").write(str(server.returncode))


sys.stdout.reconfigure(encoding="utf-8")


def ev(d):
    sys.stdout.write(json.dumps(d, ensure_ascii=False) + "\n"); sys.stdout.flush()


RESULT = {"type": "result", "subtype": "success", "is_error": False, "duration_ms": 1200,
          "duration_api_ms": 900, "num_turns": 3, "result": "done", "session_id": "s1",
          "total_cost_usd": 0.5, "usage": {"input_tokens": 10, "output_tokens": 4},
          "modelUsage": {"claude-x": {"inputTokens": 10}}, "permission_denials": [], "uuid": "u2"}
ev({"type": "system", "subtype": "init", "cwd": os.getcwd(), "session_id": "s1",
    "tools": ["Bash", "mcp__ck3__observe"], "mcp_servers": [{"name": "ck3", "status": "connected"}],
    "model": "claude-x", "permissionMode": "default", "slash_commands": [], "apiKeySource": "none",
    "claude_code_version": "9.9.9", "output_style": "default", "agents": [], "uuid": "u1"})
ev({"type": "assistant", "message": {"content": [{"type": "text", "text": "Jórvik"}]}})
sys.stdout.flush()
sys.stdout.buffer.write(b'{"type":"assistant","message":{"content":[{"type":"text","text":"bad \xff byte"}]}}\n')
sys.stdout.buffer.flush()
if mode == "finish":
    ev(RESULT); stop_server(); sys.exit(0)
if mode == "noresult":
    stop_server(); sys.exit(0)


def on_int(*_):
    ev({**RESULT, "subtype": "error_during_execution", "is_error": True})
    stop_server(); sys.exit(0)


def on_term(*_):
    stop_server(); sys.exit(143)


signal.signal(signal.SIGINT, on_int if mode == "hang-result" else signal.SIG_IGN)
signal.signal(signal.SIGTERM, on_term)
while True:
    time.sleep(0.2)
'''


@pytest.fixture
def play(install, supported, tmp_path, monkeypatch):
    """A set-up play dir, the fake claude first on PATH, short stop timeouts."""
    ck3, _ = install
    play = tmp_path / "play"
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 0
    bindir = tmp_path / "bin"
    bindir.mkdir()
    claude = bindir / "claude"
    claude.write_text(FAKE_CLAUDE.replace("@PY@", sys.executable))
    claude.chmod(0o755)
    monkeypatch.setenv("PATH", f"{bindir}{os.pathsep}{os.environ['PATH']}")
    # The server (python -m ck3agent serve) needs this tree and this interpreter's
    # site dirs: HOME is private here, so a user-site install would not be found.
    monkeypatch.setenv("PYTHONPATH", os.pathsep.join([str(ROOT)] + [p for p in sys.path if p and Path(p).is_dir()]))
    monkeypatch.setenv("FAKE_ARGV", str(tmp_path / "argv.json"))
    for k, v in (("STOP_GRACE", 1.0), ("TERM_GRACE", 5.0), ("RELEASE_WAIT", 10.0)):
        monkeypatch.setattr(cli, k, v)
    return play


RUN_SIDECARS = (".mcp.json", ".tools.json", ".release.json", ".session.json")


def manifest_of(play: Path) -> dict:
    runs = [p for p in (play / "runs").glob("*.json") if not p.name.endswith(RUN_SIDECARS)]
    assert len(runs) == 1, runs
    return json.loads(runs[0].read_text())


def server_rc(tmp_path: Path) -> int:
    return int((tmp_path / "argv.json.server_rc").read_text())


ARMED = "release on exit armed"           # mcp_server logs it once a signal starts the release
LAST = {"42": cli.LAST_CLAIM}             # the bridge refuses to empty a control session (ck-cwal)
GONE_OK = {"step": "release", "ok": True, "detail": "no ruler bound"}


def gone(r: dict) -> bool:
    """The session closed and the bridge dropped it with its claims: nothing was resumed."""
    return r["resume_status"] == "session_gone" and r["claims"] == [] and r["resumed"] is False


def test_run_records_the_session(play, tmp_path, monkeypatch):
    action_log = Path(br.action_log_path())
    action_log.parent.mkdir(parents=True)
    action_log.write_text('{"action":"before"}\n')
    monkeypatch.setenv("FAKE_ACTION_LOG", str(action_log))
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))

    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "2"]) == 0
    m = manifest_of(play)
    assert m["model"] == "claude-x" and m["session_id"] == "s1" and m["result_event"] is True
    assert m["cli_version"] == "9.9.9" and m["num_turns"] == 3 and m["result"] == "success"
    assert m["total_cost_usd"] == 0.5 and m["usage"] == {"input_tokens": 10, "output_tokens": 4}
    assert m["exit_code"] == 0 and m["claude_stop"] == "exited" and m["stop_reason"] is None
    assert m["actions"]["lines"] == 1
    stamp = Path(m["prompt_file"]).name.split(".")[0]
    runs = play / "runs"
    assert (runs / f"{stamp}.actions.jsonl").read_text() == '{"action":"during"}\n'
    lines = (runs / f"{stamp}.jsonl").read_text(encoding="utf-8").splitlines()
    assert len(lines) == 4 and "Jórvik" in lines[1] and "bad � byte" in lines[2]
    argv = json.loads((tmp_path / "argv.json").read_text())
    assert "--strict-mcp-config" in argv and argv[argv.index("--max-turns") + 1] == "400"
    assert m["teardown"] == [{"step": "bridge", "ok": False, "detail": "socket not listening"}]
    # Written by the real `ck3agent serve`, not by the fake.
    surface = json.loads((runs / f"{stamp}.tools.json").read_text())
    assert surface["schemas_via"] == "fastmcp.list_tools"
    assert [s["name"] for s in surface["schemas"]] == surface["tools"] and surface["tools"]
    assert all(s["inputSchema"]["type"] == "object" and s.get("description") for s in surface["schemas"])
    assert m["tool_surface"]["tools"] == surface["tools"]
    assert m["tool_surface"]["schemas_sha256"] == surface["schemas_sha256"]
    # Claude Code's SIGINT started the release; there was no bridge to release from.
    assert m["server_release"]["trigger"] == "SIGINT"
    assert m["server_release"]["error"].startswith("ConnectionError")
    assert m["resumed_release"] is None                   # no bridge session, so no session record


def test_server_releases_inside_claude_codes_stop_window(play, bridge, tmp_path, monkeypatch):
    """SIGINT, SIGTERM at 100 ms, SIGKILL at 500 ms: the server's own release must fit.

    Its one ruler is its last claim, which the bridge refuses to release; the
    session's close frees it (free play), and the teardown confirms that."""
    fb = bridge()
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    assert cli.main(["run", "--quiet"]) == 0
    m = manifest_of(play)
    assert server_rc(tmp_path) == 128 + signal.SIGINT, (tmp_path / "server.log").read_text()[-2000:]
    rel = m["server_release"]
    assert rel["trigger"] == "SIGINT" and rel["released"] == [] and rel["refused"] == LAST
    assert rel["held_after"] == [42] and rel["error"] is None
    assert gone(m["resumed_release"]) and fb.resumes == []
    assert fb.owner == {} and m["claude_stop"] == "exited" and m["result_event"] is True
    assert GONE_OK in m["teardown"]
    assert not (play / "runs" / (Path(m["prompt_file"]).name.split(".")[0] + ".session.json")).exists()


def test_a_single_rulers_refused_release_is_reported_ok(play, bridge, tmp_path, monkeypatch, capsys):
    bridge()
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    capsys.readouterr()
    assert cli.main(["run", "--quiet"]) == 0
    out = capsys.readouterr().out
    assert manifest_of(play)["server_release"]["refused"] == LAST       # the record keeps the raw refusal
    assert "[warn]" not in out, out
    assert "MCP server release: last ruler 42 stays bound until the session closes" in out, out
    assert re.search(r"session release: session \S+ already closed, nothing held", out), out
    assert "resumed" not in out and "teardown release: no ruler bound" in out, out


def test_a_refusal_for_any_other_reason_still_warns(play, bridge, tmp_path, monkeypatch, capsys):
    fb = bridge(replies={"release_character": {"success": False, "reason": "game_paused"}})
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    capsys.readouterr()
    assert cli.main(["run", "--quiet"]) == 0
    out = capsys.readouterr().out
    assert manifest_of(play)["server_release"]["refused"] == {"42": "game_paused"}
    assert "[warn] MCP server release: released [], refused {'42': 'game_paused'}" in out, out
    assert "stays bound" not in out and fb.owner == {}, out


def test_a_server_holding_two_rulers_releases_all_but_its_last(play, bridge, tmp_path, monkeypatch, capsys):
    """Releasing one of several claims is allowed; only the last is refused.
    The second ruler is ADDED (`session(action="add")`): `control` would switch."""
    fb = bridge()
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("FAKE_MCP_CALL", json.dumps(
        {"name": "session", "arguments": {"action": "add", "character_id": 43}}))
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    capsys.readouterr()
    assert cli.main(["run", "--quiet"]) == 0
    out = capsys.readouterr().out
    m = manifest_of(play)
    log = (tmp_path / "server.log").read_text()[-3000:]
    claimer = next(s for s, a, p in fb.log if a == "set_controlled_character")
    claims = [p["character_id"] for s, a, p in fb.log if a == "set_controlled_character"]
    assert claims == [42, 43], (claims, log)
    rel = m["server_release"]
    assert rel["session_id"] == claimer and rel["released"] == [42], (rel, log)
    assert rel["refused"] == {"43": cli.LAST_CLAIM} and rel["held_after"] == [43] and rel["error"] is None
    assert [(s, p) for s, a, p in fb.log if a == "release_character"] == [
        (claimer, {"character_id": 42}), (claimer, {"character_id": 43})]
    assert gone(m["resumed_release"]) and fb.owner == {} and GONE_OK in m["teardown"]
    assert "[warn]" not in out and "released [42]; last ruler 43 stays bound" in out, out


def test_a_prebound_list_binds_every_ruler_and_the_manifest_records_them(
        play, bridge, tmp_path, monkeypatch, capsys):
    """lane-multi: CK3_AGENT_CHARACTER_ID=42,43 binds 42 as the primary and ADDS 43.
    The teardown reads clean: 42 released by the server, 43 its last claim, freed
    when the session closed; the manifest lists both at start and at stop."""
    fb = bridge()
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42,43")
    capsys.readouterr()
    assert cli.main(["run", "--quiet"]) == 0
    out = capsys.readouterr().out
    m = manifest_of(play)
    log = (tmp_path / "server.log").read_text()[-3000:]
    binds = [p for s, a, p in fb.log if a == "set_controlled_character"]
    assert [(p["character_id"], p.get("add")) for p in binds] == [(42, None), (43, True)], (binds, log)
    rel = m["server_release"]
    assert rel["released"] == [42] and rel["refused"] == {"43": cli.LAST_CLAIM}, (rel, log)
    # lane-prebind: `bound_at` is the reply's clock stamp (the fake bridge stamps
    # none), `prebind_wait` says the game was active at the handshake (no wait).
    assert m["rulers"] == {"declared": [42, 43], "bound_at_start": [42, 43], "refused_at_start": {},
                           "bound_at": {"42": {"tick": None, "date": None},
                                        "43": {"tick": None, "date": None}},
                           "prebind_wait": {"waited_s": 0.0, "polls": 0, "game_active": True},
                           "held_at_stop": [42, 43], "held_after_teardown": []}, m["rulers"]
    assert gone(m["resumed_release"]) and fb.owner == {} and GONE_OK in m["teardown"]
    assert "[warn]" not in out, out
    assert "released [42]; last ruler 43 stays bound" in out, out
    assert "rulers: bound at start [42, 43]; held at stop [42, 43]" in out, out


def test_a_ruler_refused_at_start_is_a_warning_in_the_run_report(play, bridge, tmp_path, monkeypatch, capsys):
    fb = bridge()
    fb.owner[43] = "elsewhere"                  # another session already holds 43
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42,43")
    capsys.readouterr()
    assert cli.main(["run", "--quiet"]) == 0
    out = capsys.readouterr().out
    rl = manifest_of(play)["rulers"]
    assert rl["bound_at_start"] == [42] and rl["refused_at_start"] == {"43": "owned_by_other"}, rl
    assert "[warn] rulers: bound at start [42], refused {'43': 'owned_by_other'}" in out, out


def test_a_server_killed_before_it_releases_is_released_by_resuming_its_session(
        play, bridge, tmp_path, monkeypatch):
    # The server's snapshot takes 2 s, so Claude Code's SIGKILL lands mid-release.
    fb = bridge(delays={("ck3agent", "get_bridge_state"): 2.0})
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    assert cli.main(["run", "--quiet"]) == 0
    m = manifest_of(play)
    assert server_rc(tmp_path) == -signal.SIGKILL and m["server_release"] is None
    claimer = next(s for s, a, p in fb.log if a == "set_controlled_character")
    # The kill closed its socket and the bridge dropped the session with its claim.
    r = m["resumed_release"]
    assert r["session_id"] == claimer and gone(r)
    assert not any(a == "release_character" for s, a, p in fb.log)
    assert fb.owner == {} and fb.resumes == []
    assert GONE_OK in m["teardown"]


@pytest.mark.parametrize("drop", ["pin", "destroy"])
def test_a_reconnected_server_killed_mid_release_is_released_through_its_current_session(
        play, bridge, tmp_path, monkeypatch, drop):
    """The bridge drops the server after its pre-bind; a tool call reconnects and
    re-claims; then Claude Code's stop SIGKILLs the server mid-release (slow
    get_bridge_state). `ck3agent run` must look up the session that holds the claim.
    "pin": the server resumes its old session. "destroy": the bridge erased it, the
    resume is refused, a fresh session claims, and only the re-record names it.
    Either way the kill closes that session and the bridge frees the claim."""
    fb = bridge(delays={("ck3agent", "get_bridge_state"): 2.0}, drop_after_claim={"ck3agent": drop})
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("FAKE_MCP_CALL", json.dumps({"name": "intent", "arguments": {"action": "list"}}))
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    assert cli.main(["run", "--quiet"]) == 0
    m = manifest_of(play)
    log = (tmp_path / "server.log").read_text()[-3000:]
    assert json.loads((tmp_path / "argv.json.mcp").read_text())["id"] == 2, log
    claimers = [s for s, a, p in fb.log if a == "set_controlled_character"]
    assert len(claimers) == 2 and len(fb.dropped) == 1, (claimers, fb.dropped, log)
    first, current = claimers
    assert fb.resumes[0] == ((f"tok-{first}", True, "ok") if drop == "pin"
                             else (f"tok-{first}", False, "unknown_token"))
    assert (current == first) == (drop == "pin")
    assert server_rc(tmp_path) == -signal.SIGKILL and m["server_release"] is None, log
    r = m["resumed_release"]
    assert r["session_id"] == current and gone(r), (r, log)
    assert fb.owner == {} and len(fb.resumes) == 1, (fb.owner, fb.resumes, log)
    assert GONE_OK in m["teardown"]


def test_a_claim_in_flight_at_the_servers_snapshot_is_still_released(play, bridge, tmp_path, monkeypatch):
    # lag 1: the server's first snapshot misses the claim, its second round finds it.
    fb = bridge(snapshot_lag={"ck3agent": 1})
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    assert cli.main(["run", "--quiet"]) == 0
    m = manifest_of(play)
    rel = m["server_release"]
    assert rel["rounds"] == 3 and rel["claims"] == [42] and rel["refused"] == LAST
    assert rel["held_after"] == [42] and rel["released"] == []
    assert gone(m["resumed_release"]) and fb.owner == {} and GONE_OK in m["teardown"]


def test_a_claim_the_server_never_sees_is_released_after_it_exits(play, bridge, tmp_path, monkeypatch):
    fb = bridge(snapshot_lag={"ck3agent": 100})
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    assert cli.main(["run", "--quiet"]) == 0
    m = manifest_of(play)
    assert m["server_release"]["claims"] == [] and m["server_release"]["error"] is None
    assert gone(m["resumed_release"]) and fb.owner == {} and GONE_OK in m["teardown"]


def test_wall_seconds_stop_releases_the_ruler_through_its_own_session(play, bridge, tmp_path, monkeypatch):
    fb = bridge()
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("FAKE_MODE", "hang-ignore")                # ignores SIGINT, honours SIGTERM
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")            # the server pre-binds 42

    # The server takes ~6 s to arm (its pre-bind waits out a 5 s timeout), so stop after it has.
    assert cli.main(["run", "--quiet", "--wall-seconds", "10"]) == 0
    m = manifest_of(play)
    claimer = next(s for s, a, p in fb.log if a == "set_controlled_character")
    assert (claimer, "release_character", {"character_id": 42}) in fb.log
    assert fb.owner == {}, (tmp_path / "server.log").read_text()[-3000:]
    assert m["stop_reason"] == "wall_seconds" and m["exit_code"] == 0 and m["claude_stop"] == "SIGTERM"
    rel = m["server_release"]
    assert rel["released"] == [] and rel["refused"] == LAST and rel["held_after"] == [42]
    assert rel["trigger"] == "SIGINT" and rel["session_id"] == claimer
    assert gone(m["resumed_release"]) and GONE_OK in m["teardown"]
    assert fb.paused
    assert m["result_event"] is False
    assert m["usage"] == m["total_cost_usd"] == "unrecorded: terminated"
    # One CLI session served the start date, then one the watcher, release check, end and teardown.
    cli_sessions = [s for s, v in {**fb.sessions, **fb.destroyed}.items() if v["client_name"] == "ck3agent-cli"]
    assert len(cli_sessions) == 2 == m["bridge_sessions_opened"], fb.sessions


def test_sigint_lets_claude_write_its_result(play, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "hang-result")
    assert cli.main(["run", "--no-game-check", "--quiet", "--wall-seconds", "2"]) == 0
    m = manifest_of(play)
    assert m["claude_stop"] == "SIGINT" and m["stop_reason"] == "wall_seconds"
    assert m["result_event"] is True and m["result"] == "error_during_execution" and m["total_cost_usd"] == 0.5


def test_a_stream_without_a_result_event_says_so(play, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "noresult")
    assert cli.main(["run", "--no-game-check", "--quiet"]) == 0
    m = manifest_of(play)
    assert m["result_event"] is False and m["claude_stop"] == "exited"
    assert all(m[k] == "unrecorded: no result event" for k in cli.UNRECORDED)


@pytest.mark.parametrize("sig", [signal.SIGTERM, signal.SIGHUP])
def test_a_signal_to_ck3agent_still_stops_claude_and_writes_the_manifest(play, tmp_path, monkeypatch, sig):
    monkeypatch.setenv("FAKE_MODE", "hang-result")
    p = subprocess.Popen([sys.executable, "-m", "ck3agent", "run", "--no-game-check", "--quiet"],
                         cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = time.monotonic() + 30
    while not (tmp_path / "argv.json").exists():                 # claude is up
        assert time.monotonic() < deadline and p.poll() is None
        time.sleep(0.1)
    time.sleep(0.5)
    p.send_signal(sig)
    out, _ = p.communicate(timeout=60)
    assert p.returncode == 128 + sig, out
    m = manifest_of(play)
    assert m["stop_reason"] == f"signal {sig.name}" and m["claude_stop"] == "SIGINT"
    assert m["result_event"] is True and m["exit_code"] == 128 + sig
    claude_pid = int((tmp_path / "argv.json.pid").read_text())
    assert not Path(f"/proc/{claude_pid}").exists()             # stopped and reaped, not orphaned


TTY_CHILD = ("import fcntl, os, sys, termios; fcntl.ioctl(0, termios.TIOCSCTTY, 0); "
             "os.execv(sys.executable, [sys.executable, '-m', 'ck3agent', *sys.argv[1:]])")


@pytest.mark.parametrize("quiet", [True, False])
def test_a_terminal_hangup_still_tears_down_and_writes_the_manifest(play, bridge, tmp_path, monkeypatch, quiet):
    """A real hangup: ck3agent on a pty as its controlling terminal, then the master closed."""
    import pty
    fb = bridge()
    monkeypatch.setenv("FAKE_MODE", "hang-result")          # writes its result on SIGINT, echoed to a dead tty
    master, slave = pty.openpty()
    p = subprocess.Popen([sys.executable, "-c", TTY_CHILD, "run", *(["--quiet"] if quiet else [])],
                         cwd=ROOT, stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
    os.close(slave)
    seen, deadline = b"", time.monotonic() + 30
    try:
        while not (tmp_path / "argv.json").exists() or (not quiet and "Jórvik".encode() not in seen):
            assert time.monotonic() < deadline and p.poll() is None, seen
            import select
            if select.select([master], [], [], 0.1)[0]:
                seen += os.read(master, 65536)
        time.sleep(0.3)
    finally:
        os.close(master)                                      # the hangup: SIGHUP, then EIO on every write
    assert p.wait(60) == 128 + signal.SIGHUP
    m = manifest_of(play)
    assert m["stop_reason"] == "signal SIGHUP" and m["exit_code"] == 128 + signal.SIGHUP
    assert m["teardown"][0] == {"step": "pause", "ok": True, "detail": ""} and fb.paused
    assert m["claude_stop"] == "SIGINT" and m["result_event"] is True
    claude_pid = int((tmp_path / "argv.json.pid").read_text())
    assert not Path(f"/proc/{claude_pid}").exists()


class _PopenProc:
    """What cmd_run needs of a Popen: a finished stdout, wait/poll/send_signal."""

    def __init__(self):
        import io
        self.stdout, self.sent, self.returncode, self.pid = io.StringIO(""), [], None, 0

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        if self.returncode is None:
            raise subprocess.TimeoutExpired("claude", timeout)
        return self.returncode

    def send_signal(self, sig):
        self.sent.append(sig)
        self.returncode = -sig


def test_a_signal_inside_popen_still_stops_claude(play, monkeypatch):
    """SIGTERM between fork and `proc =`: the started claude must still be stopped."""
    made = []

    def popen(*a, **k):
        made.append(_PopenProc())
        os.kill(os.getpid(), signal.SIGTERM)                  # handled at the next bytecode, inside here
        return made[0]

    import types                                              # only cmd_run's Popen, not subprocess.run's
    sp = types.SimpleNamespace(**{k: getattr(subprocess, k) for k in dir(subprocess) if not k.startswith("__")})
    sp.Popen = popen
    monkeypatch.setattr(cli, "subprocess", sp)
    assert cli.main(["run", "--no-game-check", "--quiet"]) == 128 + signal.SIGTERM
    assert made and made[0].sent == [signal.SIGINT]
    m = manifest_of(play)
    assert m["stop_reason"] == "signal SIGTERM" and m["claude_stop"] == "SIGINT"


def test_run_stamps_are_unique_within_a_second(tmp_path):
    from datetime import datetime
    now = datetime(2026, 10, 18, 12, 0, 0)
    runs = tmp_path / "runs"
    runs.mkdir()
    got = [cli._claim_stamp(runs, now) for _ in range(3)]
    assert got == ["20261018-120000", "20261018-120000-2", "20261018-120000-3"]
    assert sorted(p.name for p in runs.iterdir()) == sorted(f"{s}.prompt.md" for s in got)


def test_write_tool_surface_record_has_the_served_schemas(tmp_path, monkeypatch):
    from ck3agent import bootstrap_server
    bootstrap_server()
    import mcp_server
    out = tmp_path / "t.json"
    monkeypatch.setenv("CK3_TOOL_SURFACE_OUT", str(out))
    mcp_server._write_tool_surface_record()
    d = json.loads(out.read_text())
    import asyncio
    served = [t.model_dump(mode="json", by_alias=True, exclude_none=True)
              for t in asyncio.run(mcp_server.mcp.list_tools())]
    assert d["schemas"] == sorted(served, key=lambda s: s["name"])
    assert d["tools"] == [s["name"] for s in d["schemas"]] and d["schemas_sha256"]


def test_summarize_stream_reads_real_event_shapes():
    init = {"type": "system", "subtype": "init", "cwd": "/p", "session_id": "a1",
            "tools": ["Bash", "mcp__ck3__observe"], "mcp_servers": [{"name": "ck3", "status": "connected"}],
            "model": "claude-opus-x", "permissionMode": "default", "slash_commands": [],
            "apiKeySource": "none", "claude_code_version": "2.1.302", "output_style": "default",
            "uuid": "u"}
    result = {"type": "result", "subtype": "error_max_turns", "is_error": True, "duration_ms": 5,
              "duration_api_ms": 4, "num_turns": 400, "session_id": "a1", "total_cost_usd": 1.25,
              "usage": {"input_tokens": 1}, "modelUsage": {"claude-opus-x": {}}, "uuid": "v"}
    m = cli.summarize_stream(["not json", "[]", json.dumps(init), json.dumps(result)])
    assert m["model"] == "claude-opus-x" and m["cli_version"] == "2.1.302"
    assert m["permission_mode"] == "default" and m["init_tools"] == ["Bash", "mcp__ck3__observe"]
    assert m["mcp_servers"] == [{"name": "ck3", "status": "connected"}]
    assert m["result"] == "error_max_turns" and m["num_turns"] == 400 and m["total_cost_usd"] == 1.25
    assert m["model_usage"] == {"claude-opus-x": {}} and m["duration_ms"] == 5 and m["result_event"] is True
    assert "result_event" not in cli.summarize_stream([json.dumps(init)])


# ── server import ────────────────────────────────────────────────────────────

def test_mcp_server_imports_and_registers_tools():
    from ck3agent import bootstrap_server
    bootstrap_server()
    import mcp_server  # noqa: F401
    assert mcp_server.mcp is not None
