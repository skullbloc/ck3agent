"""CLI tests. No game, no bridge binary; conftest.py makes HOME and XDG private."""
from __future__ import annotations

import hashlib
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
    assert mcp["alwaysLoad"] is True          # the 13 schemas load with the session (L18-C)
    settings = json.loads((play / ".claude/settings.json").read_text())
    hook = settings["hooks"]["UserPromptSubmit"][0]["hooks"][0]["command"]
    assert hook.split() == argv + ["hook-state"]
    assert settings["permissions"]["allow"] == cli.ALLOW == ["mcp__ck3"]
    assert (play / "CLAUDE.md").exists()
    assert f"LD_PRELOAD={so.resolve()} %command%" in out
    c = cfg.Config.load()
    assert (c.ck3_binary, c.bridge_so, c.play_dir) == (str(ck3), str(so.resolve()), str(play.resolve()))


def test_setup_takes_back_the_sleep_allowance_an_earlier_setup_wrote(install, supported, tmp_path):
    """D12: the play-dir allow-list no longer carries Bash(sleep:*): the protocol
    lets time pass with session(action="wait"), never a Bash sleep. A play
    directory set up by 2026.10 has the entry; setup removes it and keeps the
    player's own rules."""
    ck3, _ = install
    play = tmp_path / "play"
    (play / ".claude").mkdir(parents=True)
    old = {"permissions": {"allow": ["mcp__ck3", "Bash(sleep:*)", "Bash(ls:*)"]}}
    (play / ".claude/settings.json").write_text(json.dumps(old))
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 0
    s = json.loads((play / ".claude/settings.json").read_text())
    assert s["permissions"]["allow"] == ["mcp__ck3", "Bash(ls:*)"]
    assert not any("sleep" in r for r in cli.ALLOW)


def test_setup_no_allow_still_takes_back_the_sleep_allowance(install, supported, tmp_path):
    """--no-allow adds no rule of ours, and still removes the retired one."""
    ck3, _ = install
    play = tmp_path / "play"
    (play / ".claude").mkdir(parents=True)
    old = {"permissions": {"allow": ["Bash(sleep:*)", "Bash(ls:*)"]}}
    (play / ".claude/settings.json").write_text(json.dumps(old))
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play), "--no-allow"]) == 0
    s = json.loads((play / ".claude/settings.json").read_text())
    assert s["permissions"]["allow"] == ["Bash(ls:*)"]


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


def test_doctor_never_reads_a_play_dir_it_was_not_told(install, supported, hermetic, tmp_path,
                                                         monkeypatch, capsys):
    """smoke17: with fresh XDG dirs and no setup record, doctor read the host's
    ~/ck3agent-play/.mcp.json (another install's, its command gone) and failed rc 1."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    residue = hermetic / "ck3agent-play"
    residue.mkdir()
    (residue / ".mcp.json").write_text(json.dumps(
        {"mcpServers": {"ck3": {"command": "/gone/venv/bin/ck3agent", "args": ["serve"]}}}))
    assert cli.main(["doctor"]) == 0
    out = capsys.readouterr().out
    assert "/gone/venv" not in out and "ck3 MCP command missing" not in out
    assert "play directory not set up" in out and "none recorded in" in out
    # told explicitly, it reads it
    assert cli.main(["doctor", "--play-dir", str(residue)]) == 1
    assert "ck3 MCP command missing" in capsys.readouterr().out
    # and the dir setup recorded is the one read
    play = tmp_path / "play"
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 0
    capsys.readouterr()
    assert cli.main(["doctor"]) == 0
    assert f"{play} (config)" in capsys.readouterr().out


def test_doctor_reads_claude_codes_trust_record_for_the_play_dir(install, supported, hermetic, tmp_path,
                                                                  monkeypatch, capsys):
    """smoke17: every `claude -p` in a fresh play dir printed "Ignoring 1 permissions.allow
    entry ... this workspace has not been trusted". Doctor and setup now say so, from
    Claude Code's own record (read only); nothing writes it."""
    ck3, _ = install
    play = tmp_path / "play"
    assert cli.main(["setup", "--ck3", str(ck3), "--play-dir", str(play)]) == 0
    assert "accept Claude Code's trust dialog" in capsys.readouterr().out
    assert cli.main(["doctor"]) == 0
    out = capsys.readouterr().out
    assert "Claude Code has not trusted the play directory" in out
    assert "accept the trust dialog once" in out
    assert out.strip().endswith("Ready. Next: ck3agent launch --continue")
    rec = hermetic / ".claude.json"
    rec.write_text(json.dumps({"projects": {str(play.resolve()): {cli.TRUST_KEY: True}}}))
    before = rec.read_bytes()
    assert cli.main(["doctor"]) == 0
    out = capsys.readouterr().out
    assert "Claude Code trusts the play directory" in out and "has not trusted" not in out
    assert rec.read_bytes() == before
    assert cli.trust_record(play) == (True, str(rec))
    assert cli.trust_record(play, {"CLAUDE_CONFIG_DIR": "/x"})[0] is None
    rec.write_text("[")
    assert cli.trust_record(play)[0] is None


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
                                so, False, False)
    assert env["SDL_VIDEODRIVER"] == "x11" and "WAYLAND_DISPLAY" not in env and notes
    assert env["LD_PRELOAD"] == "/lib/b.so:/x.so"
    env, _ = cli.launch_env({"WAYLAND_DISPLAY": "w"}, so, False, True)
    assert env["WAYLAND_DISPLAY"] == "w" and "SDL_VIDEODRIVER" not in env
    env, _ = cli.launch_env({"WAYLAND_DISPLAY": "w", "SDL_VIDEODRIVER": "wayland"}, so, True, False)
    assert env["SDL_VIDEODRIVER"] == "wayland" and env["CK3_STRICT_BUILD_ID"] == "0"


def test_launch_flags():
    p = cli.build_parser()
    a = p.parse_args(["launch", "--continue", "--unsupported", "--", "-debug_mode"])
    assert a.cont and a.allow_unsupported and a.game_args == ["-debug_mode"] and a.wait is None
    assert p.parse_args(["launch", "--allow-unsupported-build"]).allow_unsupported


def test_stage_save_repoints_the_game_dirs_continue_game(tmp_path):
    src = tmp_path / "my world.ck3"
    src.write_bytes(b"SAV")
    ptr = cfg.game_user_dir() / "continue_game.json"
    ptr.parent.mkdir(parents=True)
    ptr.write_text(json.dumps({"title": "autosave", "desc": "d", "rawGameVersion": "1.18.3.1"}))
    dst, digest = cli.stage_save(src)
    assert dst == cfg.game_user_dir() / "save games/my world.ck3" and dst.read_bytes() == b"SAV"
    d = json.loads(ptr.read_text())
    assert (d["title"], d["desc"], d["rawGameVersion"]) == ("my world", "my world", "1.18.3.1")
    assert re.fullmatch(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", d["date"])
    assert digest == cli.sha256(src)
    with pytest.raises(ValueError):
        cli.stage_save(tmp_path / "nope.txt")


def _tree(root: Path) -> dict:
    """Every entry under ROOT: file bytes, link targets, directories."""
    out = {}
    for p in sorted(root.rglob("*")):
        k = str(p.relative_to(root))
        out[k] = ("link", os.readlink(p)) if p.is_symlink() else \
            ("dir",) if p.is_dir() else ("file", p.read_bytes(), p.stat().st_mtime_ns)
    return out


@pytest.fixture
def owner_tree():
    """The user's own CK3 user-data tree: a campaign, its autosave, a pointer, settings, a mod."""
    own = cfg.owner_game_dir()
    (own / "save games").mkdir(parents=True)
    (own / "save games/campaign.ck3").write_bytes(b"MINE")
    (own / "save games/autosave.ck3").write_bytes(b"AUTO")
    (own / "continue_game.json").write_text(json.dumps(
        {"title": "campaign", "desc": "Charles, King of the Franks", "date": "2026-10-16 19:59:58",
         "rawGameVersion": "1.18.3.1"}))
    (own / "pdx_settings.txt").write_text("fullscreen=no")
    (own / "last_save.ck3").write_bytes(b"LAST")
    (own / "mod").mkdir()
    (own / "mod/x.mod").write_text("name=x")
    (own / "logs").mkdir()
    return own


class _Popen:
    calls: list = []

    def __init__(self, argv, **kw):
        _Popen.calls.append((argv, kw))
        self.pid, self.returncode = 4242, None

    def poll(self):
        return None


def _launch(monkeypatch, *args):
    _Popen.calls = []
    monkeypatch.setattr(cli.subprocess, "Popen", _Popen)
    rc = cli.main(["launch", "--no-wait", *args])
    return rc, (_Popen.calls[0][0] if _Popen.calls else None)


def test_launch_gives_the_game_its_own_userdir_and_never_writes_the_users_tree(
        install, supported, owner_tree, tmp_path, monkeypatch):
    """smoke17: the launched game's autosave rotated the owner's save games/autosave.ck3.
    The game now runs with -userdir=<XDG_STATE_HOME>/ck3agent/game, and the user's
    tree (saves, continue_game.json, settings) is byte-identical after a launch."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    before = _tree(owner_tree)
    src = tmp_path / "observer.ck3"
    src.write_bytes(b"OBS")
    rc, argv = _launch(monkeypatch, "--save", str(src))
    assert rc == 0
    game = cfg.game_user_dir().resolve()
    assert game == (cfg.state_dir() / "game").resolve()
    assert not str(game).startswith(str(owner_tree))
    assert argv[:3] == [str(ck3), f"-userdir={game}", "--continuelastsave"]
    assert (game / "save games/observer.ck3").read_bytes() == b"OBS"
    assert json.loads((game / "continue_game.json").read_text())["title"] == "observer"
    # settings copied (a write stays private), directories the game writes are its own
    assert (game / "pdx_settings.txt").read_text() == "fullscreen=no"
    assert not (game / "pdx_settings.txt").is_symlink()
    assert (game / "mod").is_symlink() and not (game / "save games").is_symlink()
    assert not (game / "logs").is_symlink() and not (game / "last_save.ck3").exists()
    assert _tree(owner_tree) == before
    rec = json.loads((cfg.data_dir() / "state/last-launch.json").read_text())
    assert rec["game_user_dir"] == str(game) and rec["save"] == str(game / "save games/observer.ck3")


def test_launch_continue_copies_the_users_last_save_once(install, supported, owner_tree, monkeypatch):
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    before = _tree(owner_tree)
    rc, argv = _launch(monkeypatch, "--continue")
    game = cfg.game_user_dir().resolve()
    assert rc == 0 and f"-userdir={game}" in argv
    assert (game / "save games/campaign.ck3").read_bytes() == b"MINE"
    # the game played on in its own tree: the next --continue follows the game's pointer
    (game / "save games/autosave.ck3").write_bytes(b"AGENT")
    mine = {"title": "autosave", "desc": "Observing", "date": "2026-11-04 12:00:00"}
    (game / "continue_game.json").write_text(json.dumps(mine))
    rc, _ = _launch(monkeypatch, "--continue")
    assert rc == 0 and json.loads((game / "continue_game.json").read_text()) == mine
    assert _tree(owner_tree) == before


#: The keys CK3 1.18.3.1 reads from continue_game.json with no presence check
#: (reader ck3+0x75b3dc0: `title`, `desc`, `date`; a missing one faults at
#: ck3+0x80a6090, address 0x20). Spelled here, not imported: the test holds the
#: game's contract, the code's constant is what it grades.
CK3_POINTER_KEYS = ("title", "desc", "date")


def test_launch_save_writes_a_pointer_ck3_can_read(install, supported, owner_tree, tmp_path, monkeypatch):
    """smoke18: 2026.12.1 wrote {"title": ...} alone into a new game directory and
    CK3 SIGSEGVed in every launch, ~17 s after loading history (desc missing)."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    src = tmp_path / "observer.ck3"
    src.write_bytes(b"OBS")
    rc, argv = _launch(monkeypatch, "--save", str(src))
    d = json.loads((cfg.game_user_dir() / "continue_game.json").read_text())
    assert rc == 0 and d["title"] == "observer"
    assert all(isinstance(d.get(k), str) and d[k] for k in CK3_POINTER_KEYS), d


def test_launch_continue_repairs_a_pointer_2026_12_1_wrote(install, supported, owner_tree, monkeypatch, capsys):
    """A game directory 2026.12.1 left behind holds a title-only pointer: --continue
    would load nothing new, so the pointer itself must be made readable."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    game = cfg.game_user_dir()
    (game / "save games").mkdir(parents=True)
    (game / "save games/smoke18_867.ck3").write_bytes(b"S")
    (game / "continue_game.json").write_text(json.dumps({"title": "smoke18_867"}))
    rc, argv = _launch(monkeypatch, "--continue")
    d = json.loads((game / "continue_game.json").read_text())
    assert rc == 0 and d["title"] == "smoke18_867"
    assert all(isinstance(d.get(k), str) and d[k] for k in CK3_POINTER_KEYS), d
    assert "continue_game.json repaired" in capsys.readouterr().out


def test_a_plain_launch_repairs_a_pointer_2026_12_1_wrote(install, supported, owner_tree, monkeypatch, capsys):
    """Witness F6: only --continue repaired a title-only pointer; a plain `launch`
    kept it, and the main menu's Continue may go through the same reader."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    game = cfg.game_user_dir()
    (game / "save games").mkdir(parents=True)
    (game / "save games/smoke18_867.ck3").write_bytes(b"S")
    (game / "continue_game.json").write_text(json.dumps({"title": "smoke18_867"}))
    rc, argv = _launch(monkeypatch)
    d = json.loads((game / "continue_game.json").read_text())
    assert rc == 0 and "--continuelastsave" not in argv and d["title"] == "smoke18_867"
    assert all(isinstance(d.get(k), str) and d[k] for k in CK3_POINTER_KEYS), d
    assert "continue_game.json repaired" in capsys.readouterr().out
    # a pointer that names no save is left alone by a plain launch (there is nothing to name)
    (game / "continue_game.json").write_text(json.dumps({"desc": "x"}))
    rc, argv = _launch(monkeypatch)
    assert rc == 0 and json.loads((game / "continue_game.json").read_text()) == {"desc": "x"}


class _Exited(_Popen):
    def poll(self):
        self.returncode = 1
        return 1


def test_launch_waits_longer_on_a_first_start(install, supported, owner_tree, monkeypatch, capsys):
    """smoke18: a new game directory has no shader cache; the first load ran past
    the 120 s default and the warning blamed a click on Continue."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    monkeypatch.setattr(cli.subprocess, "Popen", _Exited)
    assert cli.main(["launch", "--continue"]) == 1          # the stand-in game exits at once
    out = capsys.readouterr().out
    assert "shader cache" in out and "waiting up to 600s" in out
    (cfg.game_user_dir() / "shadercache/x.bin").write_bytes(b"S")   # a second start
    assert cli.main(["launch", "--continue"]) == 1
    assert "shader cache" not in capsys.readouterr().out


_IDLER_LINE = "[08:15:20][D][gameapplication.cpp:559]: Setting idler '{}' with NO init options\n"
_HISTORY = "[13:47:11][D][history.cpp:1381]: End loading of history\n"


class _WritesLog(_Popen):
    """A stand-in game that writes its debug.log as it starts (so the log postdates the launch)."""
    text: str | None = None

    def __init__(self, argv, **kw):
        super().__init__(argv, **kw)
        if _WritesLog.text is not None:
            log = cfg.game_debug_log()
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(_WritesLog.text)


@pytest.mark.parametrize("row, log_text, want, never", [
    # the four stages a launch can time out in, read from this launch's log
    ("no_idler", _HISTORY, "CK3 is still loading", "main menu"),
    ("Frontend", _HISTORY + _IDLER_LINE.format("Frontend"), "main menu", "still loading"),
    ("Load Save", _IDLER_LINE.format("Frontend") + _IDLER_LINE.format("Load Save"),
     "CK3 is loading the save", "main menu"),
    ("In Game", _IDLER_LINE.format("Load Save") + _IDLER_LINE.format("In Game"),
     "CK3 is in game", "CK3 is loading"),
    # any other screen is named as the log names it, never called "loading the save"
    ("other", _IDLER_LINE.format("Lobby"), "'Lobby'", "loading the save"),
    # no log at all, and a previous run's log: neither is "still loading"
    ("missing", None, "could not be read", "CK3 is still loading"),
    ("stale", "STALE", "predates this launch", "CK3 is in game"),
])
def test_launch_timeout_says_what_the_game_is_doing(row, log_text, want, never, install, supported,
                                                    owner_tree, monkeypatch, capsys):
    """Witness F3: game_stage returned None for a missing log and for a log with no
    idler (both "still loading"), called every idler but Frontend/In Game "loading
    the save", and read a previous run's debug.log as this launch's."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    log = cfg.game_debug_log()
    if log_text == "STALE":     # a previous run's log, an hour old, that ended in game
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(_IDLER_LINE.format("In Game"))
        os.utime(log, (time.time() - 3600, time.time() - 3600))
        _WritesLog.text = None
    else:
        _WritesLog.text = log_text
    monkeypatch.setattr(cli.subprocess, "Popen", _WritesLog)
    calls = []
    monkeypatch.setattr(br, "alive", lambda *a, **k: calls.append(1) or len(calls) > 1)
    monkeypatch.setattr(br, "session", lambda *a, **k: (_ for _ in ()).throw(OSError("lobby_view_not_constructed")))
    monkeypatch.setattr(br.time, "sleep", lambda s: None)
    assert cli.main(["launch", "--continue", "--wait", "1"]) == cli.EXIT_TIMEOUT
    out = capsys.readouterr().out
    assert want in out and never not in out, out
    assert "click on Continue" not in out or row == "Frontend", out


@pytest.mark.parametrize("args, want, never", [
    (["--continue"], "CK3 is at its main menu: the save did not load", None),
    ([], "CK3 is at its main menu.", "click on Continue"),     # a plain launch loads no save
])
def test_launch_socket_not_up_says_what_the_game_is_doing(args, want, never, install, supported,
                                                          owner_tree, monkeypatch, capsys):
    """Witness F1: troubleshooting.md, under "bridge socket not listening", says that
    when the wait runs out `launch` reads the game's log and says what the game is
    doing; the socket-not-up branch printed no stage (only the game-not-active one did)."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    _WritesLog.text = _HISTORY + _IDLER_LINE.format("Frontend")
    monkeypatch.setattr(cli.subprocess, "Popen", _WritesLog)
    monkeypatch.setattr(br, "alive", lambda *a, **k: False)
    assert cli.main(["launch", *args, "--wait", "1"]) == cli.EXIT_TIMEOUT
    out = capsys.readouterr().out
    assert "bridge socket not up" in out and want in out, out
    assert never is None or never not in out, out


def test_a_plain_launch_waits_like_the_others():
    """L22 (smoke19): a plain `launch` returned at once (wait 0)."""
    assert cli.launch_wait(False, None, first_load=True) == cli.FIRST_LOAD_WAIT
    assert cli.launch_wait(False, None, first_load=False) == cli.CONTINUE_WAIT
    assert cli.launch_wait(True, None, first_load=True) == 0
    assert cli.launch_wait(False, 7, first_load=True) == 7


@pytest.mark.parametrize("log_text, rc, want, never", [
    # smoke19: the headless node sat after "End loading of history" with no idler
    (_HISTORY, cli.EXIT_TIMEOUT, ["game not at its main menu", "CK3 is still loading"], "game at its main menu"),
    (_HISTORY + _IDLER_LINE.format("Frontend"), 0, ["game at its main menu"], "not at its main menu"),
])
def test_a_plain_launch_says_whether_the_game_reached_its_main_menu(
        log_text, rc, want, never, install, supported, owner_tree, monkeypatch, capsys):
    """L22 (smoke19): with the socket up, a plain launch returned 0 and said
    nothing while the game never reached the menu. It now waits for the log's
    main-menu screen and, when the wait runs out, names the stage the log shows."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    _WritesLog.text = log_text
    monkeypatch.setattr(cli.subprocess, "Popen", _WritesLog)
    alive = iter([False] + [True] * 100)       # no bridge before the launch, then up
    monkeypatch.setattr(br, "alive", lambda *a, **k: next(alive))
    monkeypatch.setattr(br.time, "sleep", lambda s: None)
    assert cli.main(["launch", "--wait", "1"]) == rc
    out = capsys.readouterr().out
    assert all(w in out for w in want) and never not in out, out


def test_launch_busy_socket_names_the_holder(install, supported, monkeypatch, capsys):
    """smoke18: a crashed game's CrashReporter inherited the socket and kept it
    answering; the refusal said "another CK3 ... is running" while none ran."""
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    monkeypatch.setattr(br, "alive", lambda *a, **k: True)
    # raising=False: graded against a tree without socket_holders, the row is red
    # on the refusal's text (an assertion), not on a missing attribute.
    monkeypatch.setattr(br, "socket_holders", lambda p: [(777, "/steam/Crusader Kings III/binaries/CrashReporter")],
                        raising=False)
    rc, argv = _launch(monkeypatch)
    out = capsys.readouterr().out
    assert rc == 1 and argv is None
    assert "pid 777 (/steam/Crusader Kings III/binaries/CrashReporter)" in out and "kill 777" in out


@pytest.mark.parametrize("exe, unidentified, game", [
    ("?", True, False),                                          # /proc/<pid>/exe unreadable
    ("/steam/Crusader Kings III/binaries/ck3 (deleted)", False, True),   # binary replaced mid-run
    ("/steam/Crusader Kings III/binaries/ck3", False, True),
])
def test_busy_socket_kill_note_only_for_an_exe_read_as_not_ck3(exe, unidentified, game):
    """Witness F2: a holder whose exe could not be read ("?"), or a live game whose
    binary was replaced ("... (deleted)"), was told "pid N is not CK3 ... kill N"."""
    detail, notes = cli.busy_socket_lines("/run/s.sock", [(31, exe)])
    text = "\n".join(notes)
    assert f"pid 31 ({exe})" in detail
    assert "kill" not in text and "is not CK3" not in text, text
    assert ("could not be identified" in text) is unidentified, text
    assert ("another CK3 with the bridge is running" in text) is game, text


def test_launch_refusal_names_the_process_holding_a_real_socket(install, supported, tmp_path, monkeypatch, capsys):
    """socket_holders end to end: a real listener (this test's own process) on the
    bridge path; the refusal names its pid and exe, read from /proc by inode, and
    no longer says only "another CK3 with the bridge is running"."""
    import socket as so
    ck3, so_path = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so_path))
    path = str(tmp_path / "agent.sock")
    monkeypatch.setattr(br, "socket_path", lambda: path)
    s = so.socket(so.AF_UNIX, so.SOCK_STREAM)
    try:
        s.bind(path)
        s.listen(1)
        rc, argv = _launch(monkeypatch)
        out = capsys.readouterr().out
        assert rc == 1 and argv is None
        assert f"pid {os.getpid()} ({os.readlink('/proc/self/exe')})" in out, out
    finally:
        s.close()
    assert br.socket_holders(path) == []


def test_launch_refuses_a_userdir_it_cannot_pass(install, supported, owner_tree, tmp_path, monkeypatch, capsys):
    ck3, so = install
    monkeypatch.setenv("CK3_BINARY", str(ck3))
    monkeypatch.setenv("CK3AGENT_BRIDGE", str(so))
    rc, argv = _launch(monkeypatch, "--", "-userdir=/elsewhere")
    assert rc == 1 and argv is None and "-userdir in the game arguments" in capsys.readouterr().out
    monkeypatch.setenv("XDG_STATE_HOME", str(tmp_path / "with space"))
    rc, argv = _launch(monkeypatch)
    assert rc == 1 and argv is None and "spaces or quotes" in capsys.readouterr().out
    monkeypatch.setenv("XDG_STATE_HOME", str(tmp_path / "st"))
    (owner_tree / "continue_game.json").write_text("{}")
    rc, argv = _launch(monkeypatch, "--continue")
    assert rc == 1 and argv is None and "names no save" in capsys.readouterr().out


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
if "--resume" in args:
    # A cell's continuation: record which session was resumed; with
    # FAKE_RESUME_HORIZON the resumed session reaches the horizon and plays on.
    open(os.environ["FAKE_ARGV"] + ".resumes", "a").write(args[args.index("--resume") + 1] + "\n")
    if os.environ.get("FAKE_RESUME_HORIZON"):
        os.environ["FAKE_HORIZON"] = "reply"
        mode = "hang-result"
open(os.environ["FAKE_ARGV"] + ".pid", "w").write(str(os.getpid()))
open(os.environ["FAKE_ARGV"], "w").write(json.dumps(args))
open(os.environ["FAKE_ARGV"] + ".env", "w").write(json.dumps({"CK3_SESSION_OUT": os.environ.get("CK3_SESSION_OUT"),
                                                              "CK3_SESSION_ID": os.environ.get("CK3_SESSION_ID")}))
if os.environ.get("FAKE_ACTION_LOG"):
    open(os.environ["FAKE_ACTION_LOG"], "a").write('{"action":"during"}\n')
cfg = json.load(open(args[args.index("--mcp-config") + 1]))["mcpServers"]["ck3"]
open(os.environ["FAKE_ARGV"] + ".agent_ids", "a").write(repr(os.environ.get("CK3_AGENT_CHARACTER_ID")) + "\n")
if os.environ.get("FAKE_HELD") is not None and "--resume" not in args:
    # The first claude's server, at exit: what its session held (release record).
    open(cfg["env"]["CK3_RELEASE_ON_EXIT"], "w").write(json.dumps(
        {"claims": json.loads(os.environ["FAKE_HELD"]), "released": [], "refused": {}, "error": None}))
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
if "--resume" in args:
    # claude reports total_cost_usd CUMULATIVE over a resumed session (the v3reg2
    # bundles: one session id, 0.21, 0.24 ... 1.575 over 40 processes): each
    # continuation of s1 costs 0.3 more than the last report.
    RESULT["total_cost_usd"] = 0.5 + 0.3 * len(open(os.environ["FAKE_ARGV"] + ".resumes").read().split())
ev({"type": "system", "subtype": "init", "cwd": os.getcwd(), "session_id": "s1",
    "tools": ["Bash", "mcp__ck3__observe"], "mcp_servers": [{"name": "ck3", "status": "connected"}],
    "model": "claude-x", "permissionMode": "default", "slash_commands": [], "apiKeySource": "none",
    "claude_code_version": "9.9.9", "output_style": "default", "agents": [], "uuid": "u1"})
ev({"type": "assistant", "message": {"content": [{"type": "text", "text": "Jórvik"}]}})
sys.stdout.flush()
sys.stdout.buffer.write(b'{"type":"assistant","message":{"content":[{"type":"text","text":"bad \xff byte"}]}}\n')
sys.stdout.buffer.flush()
for _ in range(int(os.environ.get("FAKE_CYCLES", "0"))):
    for tool, inp in (("observe", {}), ("session", {"action": "wait"})):
        ev({"type": "assistant", "message": {"content": [
            {"type": "tool_use", "id": "t", "name": "mcp__ck3__" + tool, "input": inp}]}})
if os.environ.get("FAKE_HORIZON"):
    # FAKE_HORIZON=narration: a seat SAYS the word (never a stop); =reply: the
    # ck3 server's cell wait reply at the horizon (horizon_reached: true).
    ev({"type": "assistant", "message": {"content": [
        {"type": "text", "text": "the horizon_reached flag is what ends us"},
        {"type": "tool_use", "id": "hz", "name": "mcp__ck3__session", "input": {"action": "wait", "seconds": 60}}]}})
    reply = {"ok": True, "verdict": "applied", "game": {"date": "867.07.02"},
             "result": {"stopped": "horizon", "horizon_reached": os.environ["FAKE_HORIZON"] == "reply",
                        "cycle_index": 2, "game_days_advanced": 90}}
    ev({"type": "user", "message": {"content": [
        {"type": "tool_result", "tool_use_id": "hz", "content": [{"type": "text", "text": json.dumps(reply)}]}]}})
if mode == "finish":
    ev(RESULT); stop_server(); sys.exit(0)
if mode == "noresult":
    stop_server(); sys.exit(0)


def on_int(*_):
    for tool in [t for t in os.environ.get("FAKE_AFTER_INT", "").split(",") if t]:
        # A call that was in flight when the run's stop fired (the race the
        # manifest's cross-check column shows).
        ev({"type": "assistant", "message": {"content": [
            {"type": "tool_use", "id": "t", "name": "mcp__ck3__" + tool, "input": {}}]}})
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


def test_run_gives_the_hook_this_seats_session_and_loads_the_tools_eagerly(play, tmp_path):
    """L18-C: the live-state hook (claude's child) reads its ruler from the seat's
    own session record, so claude carries CK3_SESSION_OUT; and the run's ck3 entry
    says alwaysLoad, so the 13 schemas are not deferred behind ToolSearch."""
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "2"]) == 0
    m = manifest_of(play)
    stamp = Path(m["prompt_file"]).name.split(".")[0]
    env = json.loads((tmp_path / "argv.json.env").read_text())
    assert env["CK3_SESSION_OUT"] == str(play / "runs" / f"{stamp}.session.json")
    srv = json.loads((play / "runs" / f"{stamp}.mcp.json").read_text())["mcpServers"]["ck3"]
    assert srv["alwaysLoad"] is True and srv["env"]["CK3_SESSION_OUT"] == env["CK3_SESSION_OUT"]
    # The fake transcript makes no ck3 call: zero cycles, said, never absent (smoke16).
    assert m["cycles_completed"] == 0 and m["cycles_met"] is False and m["cycles"] == 2
    assert m["cycle_definition"] == cli.CYCLE_DEFINITION and m["ended_on"] is None
    assert m["total_cost_usd_source"].startswith("claude result event")


def test_the_run_stops_itself_at_its_cycle_count(play, monkeypatch):
    """L18 round 2 (owner ruling): the run prompt names no cycle count; the run
    counts cycles from the transcript and ends the session at --cycles. The fake
    plays two cycles and then would play on (it ignores nothing, writes its result
    on SIGINT); --wall-seconds is only the backstop that ends the base tree's run."""
    monkeypatch.setenv("FAKE_MODE", "hang-result")
    monkeypatch.setenv("FAKE_CYCLES", "2")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "2", "--wall-seconds", "20"]) == 0
    m = manifest_of(play)
    assert m["stop_reason"] == "cycles" and m["claude_stop"] == "SIGINT", m
    assert m["cycles_completed"] == 2 and m["cycles_met"] is True and m["exit_code"] == 0
    assert m["cycles_recounted"] == 2


def test_the_manifests_count_is_the_counter_the_stop_read(play, monkeypatch):
    """L18 round 3 (witness item 2): one counter. The stop read the live
    CycleCounter at 2 closed cycles and froze it; the fake then writes one more
    `observe` before its result (a call in flight when SIGINT landed). The
    manifest's `cycles_completed` is the counter's reading at the stop, 2; the
    transcript's recount, `cycles_recounted`, counts the late observe as an open
    cycle, 3, and is the cross-check beside it. Red on db8857cc, where the
    manifest re-read the transcript (cycles_completed 3, no cycles_recounted)."""
    monkeypatch.setenv("FAKE_MODE", "hang-result")
    monkeypatch.setenv("FAKE_CYCLES", "2")
    monkeypatch.setenv("FAKE_AFTER_INT", "observe")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "2", "--wall-seconds", "20"]) == 0
    m = manifest_of(play)
    assert m["stop_reason"] == "cycles" and m["cycles"] == 2, m
    assert m["cycles_completed"] == 2 and m["cycles_met"] is True
    assert m["cycles_recounted"] == 3
    # The late call is in the transcript (the record is whole), and ended_on is the stop's reading.
    assert m["ended_on"] == "session:wait"
    stamp = Path(m["prompt_file"]).name.split(".")[0]
    stream = (play / "runs" / f"{stamp}.jsonl").read_text(encoding="utf-8")
    assert stream.count('"name": "mcp__ck3__observe"') == 3


def test_a_frozen_counter_keeps_the_reading_the_stop_took():
    c = cli.CycleCounter()
    for line in _calls("observe", "session:wait", "observe", "session:wait"):
        c.feed(line)
    assert c.closed == 2
    assert c.freeze()["cycles_completed"] == 2
    for line in _calls("observe", "pass_turn", "session:wait"):
        c.feed(line)
    assert c.result()["cycles_completed"] == 2 and c.result()["ended_on"] == "session:wait"
    assert c.freeze() == c.result()        # a second freeze changes nothing


def test_the_run_prompt_names_no_cycle_count(play):
    """The written prompt carries no budget line for the model to count against."""
    assert "{{cycles}}" not in (cli.PROTOCOL_DIR / "run-prompt.md").read_text(encoding="utf-8")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "7"]) == 0
    prompt = Path(manifest_of(play)["prompt_file"]).read_text(encoding="utf-8")
    assert "BUDGET" not in prompt and "7 cycles" not in prompt
    assert "until the run stops you" in prompt


def _calls(*names):
    """stream-json assistant events, one tool_use each, in Claude Code's shape."""
    out = []
    for n in names:
        tool, _, action = n.partition(":")
        out.append(json.dumps({"type": "assistant", "message": {"content": [
            {"type": "tool_use", "id": "t", "name": f"mcp__ck3__{tool}",
             "input": {"action": action} if action else {}}]}}))
    return out


def test_a_cycle_is_counted_once_however_it_ends():
    """The pilots' shapes (budget0 e1, 18 cycles asked): s1 waited after its last
    cycle, s2 ended its last cycle in `pause` (the hand analyzer counted 17 wait
    groups and called it "ended_early_success"), s4 observed twice in a cycle."""
    cyc = ["observe", "plan_war", "pass_turn", "session:wait"]
    s1 = ["session:whoami", "set_phase"] + cyc * 18 + ["session:pause"]
    s2 = cyc * 17 + ["observe", "inspect", "do", "session:pause"]
    s4 = ["observe", "observe", "pass_turn", "session:wait", "session:wait"] + cyc * 16 + \
        ["observe", "pass_turn", "session:pause"]
    for seq, waits in ((s1, 18), (s2, 17), (s4, 17)):
        c = cli.count_cycles(_calls(*seq) + ["not json", "[]"])
        assert c["cycles_completed"] == 18 and c["wait_groups"] == waits, (c, seq[-3:])
        assert c["ended_on"] == "session:pause"
    # A ToolSearch or other non-ck3 call is not a cycle; waits with no observe are not either.
    other = [json.dumps({"type": "assistant", "message": {"content": [
        {"type": "tool_use", "name": "ToolSearch", "input": {}}]}})]
    assert cli.count_cycles(other + _calls("session:wait", "session:wait"))["cycles_completed"] == 0


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
    monkeypatch.setenv("FAKE_MCP_CALL", json.dumps({"name": "query", "arguments": {"domain": "realm"}}))
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


def _results(*rows):
    return [json.dumps({"type": "result", "subtype": "success", "session_id": sid,
                        "total_cost_usd": cost, "num_turns": turns}) for sid, cost, turns in rows]


def test_a_resumed_sessions_cost_is_its_last_cumulative_value_not_the_sum():
    """seat-review.md problem 15: summary.json showed resumed seats at 20-30x their
    cost because every result event's CUMULATIVE total_cost_usd was summed. The
    numbers are seat K of the sealed v3reg2-flat_hom-s3 bundle (one session
    0649e4ae, two processes: 0.8999130 then 1.6094637; turns 71 then 46); the
    bundle's summary recorded K at 2.5093767, their sum. Red on 8cef520a: 2.5093767."""
    m = cli.summarize_stream(_results(("0649e4ae", 0.8999129999999997, 71), ("0649e4ae", 1.6094637, 46)))
    assert m["total_cost_usd"] == 1.6094637 and m["num_turns"] == 117
    assert m["total_cost_usd_rule"] == cli.COST_RULE
    # two sessions (a fresh start after a lost one): each session's last value, summed
    m = cli.summarize_stream(_results(("a", 0.2, 3), ("a", 0.5, 3), ("b", 0.1, 2), ("b", 0.4, 2)))
    assert abs(m["total_cost_usd"] - 0.9) < 1e-9 and m["num_turns"] == 10


def test_a_session_whose_cost_goes_down_is_not_established_never_a_number():
    m = cli.summarize_stream(_results(("a", 0.5, 3), ("a", 0.2, 3)))
    assert isinstance(m["total_cost_usd"], str) and m["total_cost_usd"].startswith("not_established")


# ── server import ────────────────────────────────────────────────────────────

def test_mcp_server_imports_and_registers_tools():
    from ck3agent import bootstrap_server
    bootstrap_server()
    import mcp_server  # noqa: F401
    assert mcp_server.mcp is not None


def test_the_run_stops_itself_when_its_server_says_the_horizon_is_reached(play, monkeypatch):
    """2026.12.4 (PLAN §P decision 4): under a cell the stop is the server's
    `horizon_reached`, not --until-date: a `session` reply of the run's ck3 server
    carrying `horizon_reached: true` stops the run (stop_reason "horizon", exit 0).
    The fake would play on; --wall-seconds is only the backstop that ends the base
    tree's run (red on 08bb8f92: stop_reason "wall_seconds")."""
    monkeypatch.setenv("FAKE_MODE", "hang-result")
    monkeypatch.setenv("FAKE_HORIZON", "reply")
    monkeypatch.setenv("CK3_EPISODE_HORIZON_END", "867.07.02")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    m = manifest_of(play)
    assert m["stop_reason"] == "horizon" and m["claude_stop"] == "SIGINT", m
    assert m["horizon_reached"] is True and m["horizon_reached_game_date"] == "867.07.02"
    assert m["horizon_end"] == "867.07.02" and m["exit_code"] == 0


def test_a_seat_saying_horizon_is_not_the_servers_reply(play, monkeypatch):
    """The control: the same transcript with the reply's flag false and the word in
    the seat's narration does not stop the run; the backstop does."""
    monkeypatch.setenv("FAKE_MODE", "hang-result")
    monkeypatch.setenv("FAKE_HORIZON", "narration")
    monkeypatch.setenv("CK3_EPISODE_HORIZON_END", "867.07.02")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "3"]) == 0
    m = manifest_of(play)
    assert m["stop_reason"] == "wall_seconds" and m["horizon_reached"] is False, m


def test_the_run_names_the_seat_session_for_the_lineage_rows(play, tmp_path, monkeypatch):
    """2026.12.4 (cell0 gap 11): lineage rows carry seat_session_id from
    CK3_SESSION_ID, and `ck3agent run` set none (null in every row). The run now
    sets it, to the harness's value when one is declared, else "run-<stamp>"; the
    server's entry and claude's environment carry the same value, and the manifest
    says which source. Red on 08bb8f92 (no CK3_SESSION_ID anywhere)."""
    monkeypatch.delenv("CK3_SESSION_ID", raising=False)
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "1"]) == 0
    m = manifest_of(play)
    stamp = Path(m["prompt_file"]).name.split(".")[0]
    env = json.loads((tmp_path / "argv.json.env").read_text())
    srv = json.loads((play / "runs" / f"{stamp}.mcp.json").read_text())["mcpServers"]["ck3"]
    assert env["CK3_SESSION_ID"] == srv["env"]["CK3_SESSION_ID"] == f"run-{stamp}"
    assert m["seat_session_id"] == f"run-{stamp}" and m["seat_session_id_source"] == "run_stamp"


def test_a_declared_seat_session_id_wins(play, tmp_path, monkeypatch):
    monkeypatch.setenv("CK3_SESSION_ID", "dry-K-13742")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "1"]) == 0
    m = manifest_of(play)
    env = json.loads((tmp_path / "argv.json.env").read_text())
    assert env["CK3_SESSION_ID"] == "dry-K-13742"
    assert m["seat_session_id"] == "dry-K-13742" and m["seat_session_id_source"] == "environment"


def _server_module(name: str):
    import importlib.util
    spec = importlib.util.spec_from_file_location(f"_{name}_mirror", ROOT / "ck3agent" / "server" / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod              # dataclasses resolve their module by name
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.modules.pop(spec.name, None)
    return mod


def test_the_runs_env_spellings_are_the_servers():
    """One spelling per variable, each mirror checked: the run's horizon start
    and end are cycle_cadence's, the seat's session id, lineage sink and sink
    row kinds are seat_lineage's, the declared rulers are mcp_server's."""
    cc, sl = _server_module("cycle_cadence"), _server_module("seat_lineage")
    assert getattr(cli, "LINEAGE_OUT_ENV", None) == sl.LINEAGE_OUT_ENV
    assert getattr(cli, "LINEAGE_RULER_KINDS", None) == (sl.KIND_BOUND, sl.KIND_REBIND)
    assert getattr(cli, "LINEAGE_END_KINDS", None) == (sl.KIND_LOST, sl.KIND_UNBOUND)
    from ck3agent import bootstrap_server
    bootstrap_server()
    import mcp_server
    assert getattr(cli, "AGENT_CHARACTER_ENV", None) == mcp_server.AGENT_CHARACTER_ENV
    assert cli.HORIZON_END_ENV == cc.HORIZON_END_ENV
    assert getattr(cli, "HORIZON_START_ENV", None) == cc.HORIZON_START_ENV
    assert cli.SESSION_ID_ENV == getattr(sl, "SESSION_ID_ENV", None)


CELL_ENV = {"CK3_EPISODE_HORIZON_START": "867.01.03", "CK3_EPISODE_HORIZON_END": "867.07.02",
            "CK3_CYCLE_GAME_DAYS": "90"}


def _resumes(tmp_path: Path) -> list[str]:
    p = tmp_path / "argv.json.resumes"
    return p.read_text().split() if p.exists() else []


def test_a_cell_seat_that_ends_its_session_early_is_resumed(play, tmp_path, monkeypatch):
    """Free running (owner ruling; L23 witness item 2): the dry cell's dukes ended
    their sessions (`end_turn` after a cycle summary) at 867.04, and the run
    recorded them as finished. Under a cell the run now resumes the session
    (`claude --resume <id>`, the same flags, the continuation text); here the
    resumed session reaches the horizon and the run stops on it. Red on 85c6dd98:
    stop_reason None, nothing resumed."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("FAKE_RESUME_HORIZON", "1")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    m = manifest_of(play)
    assert m["stop_reason"] == "horizon" and m["horizon_reached"] is True, m
    assert m["cell"] is True and len(m["continuations"]) == 1
    c = m["continuations"][0]
    assert c["claude_stop"] == "exited" and c["claude_rc"] == 0 and c["result"] == "success"
    assert c["resumed_session_id"] == "s1" and "utc" in c and "game_date" in c
    assert _resumes(tmp_path) == ["s1"]
    argv = json.loads((tmp_path / "argv.json").read_text())
    assert argv[argv.index("-p") + 1] == cli.CONTINUATION_TEXT
    assert "--strict-mcp-config" in argv and argv[argv.index("--max-turns") + 1] == "400"
    # two claude processes, two result events of ONE session: the turns are their sum,
    # the cost is the session's last cumulative value (cli.COST_RULE), not 0.5 + 0.8
    assert m["result_events"] == 2 and m["num_turns"] == 6
    assert m["total_cost_usd"] == 0.8 and m["total_cost_usd_rule"] == cli.COST_RULE


def _turns(play: Path) -> list[dict]:
    files = list((play / "runs").glob("*" + cli.TURNS_SUFFIX))
    assert len(files) == 1, files
    return [json.loads(x) for x in files[0].read_text().splitlines() if x.strip()]


def test_every_turns_prompt_is_recorded_beside_the_transcript(play, tmp_path, monkeypatch):
    """Lane v4sees (seat review problem 11: the seat review could not see what a
    resumed seat was told). Turn 0 is the run's prompt, turn k the k-th
    continuation's, each verbatim with its sha256, game date and cycle; the
    manifest names the file and counts its rows. Red on 8cef520a: no
    TURNS_SUFFIX, no record."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("FAKE_RESUME_HORIZON", "1")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    m = manifest_of(play)
    rows = _turns(play)
    assert [r["turn"] for r in rows] == [0, 1] and [r["kind"] for r in rows] == ["initial", "continuation"]
    assert rows[0]["text"] == Path(m["prompt_file"]).read_text(encoding="utf-8")
    assert rows[1]["text"] == cli.CONTINUATION_TEXT and rows[1]["resumed_session_id"] == "s1"
    for r in rows:
        assert r["text_sha256"] == hashlib.sha256(r["text"].encode()).hexdigest()
        assert "cycle" in r and "game_date" in r and r["cycle_state"]
    assert rows[1]["delivery"] == {"state": "no_channel"} and rows[1]["messages_delivered"] == []
    assert m["turns_file"].endswith(cli.TURNS_SUFFIX) and m["turns_recorded"] == 2
    assert m["hook_file"].endswith(cli.HOOK_SUFFIX)
    assert m["continuations"][0]["turn"] == 1 and m["continuations"][0]["delivery"] == "no_channel"


def test_a_resumed_turn_carries_the_seats_pending_messages_once(play, tmp_path, monkeypatch):
    """Lane v4sees (seat review problem 9: messages reached a seat only inside
    observe; s6 D1 read one 530 days late). A resumed turn's prompt carries the
    seat's pending messages after the continuation text, through the channel's
    one delivery function, which stamps the receipt (via resume_prompt, the
    turn): observe never shows them again. Red on 8cef520a: the prompt is the
    bare continuation text and the message stays unread."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    eid = "ep-20261111T000000Z-turns-0badf00d"
    for k, v in {"CK3_EPISODE_ARM": "council", "CK3_CHANNEL_TOPOLOGY": "complete",
                 "CK3_CHANNEL_QUOTA_PER_CYCLE": "2", "CK3_CHANNEL_MAX_TEXT_BYTES": "600",
                 "CK3_CHANNEL_SEATS": "K,D1", "CK3_SEAT_LABEL": "D1", "CK3_EPISODE_ID": eid,
                 "CK3_INSTANCE_ID": "turns-test", "CK3_INSTANCE_ROOT": str(tmp_path / "inst")}.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("FAKE_RESUME_HORIZON", "1")
    from ck3agent import bootstrap_server
    bootstrap_server()
    import channel
    king = channel.ChannelConfig(arm="council", topology="complete", seats=("K", "D1"), seat="K",
                                 quota_per_cycle=2, max_text_bytes=600)
    ledger = channel.Ledger(channel.ledger_path(eid), episode_id=eid)
    sent = channel.attempt(king, ledger, to="D1", text="join my war on Ludwig", game_date="869.03.01",
                           tick=5, cycle=7)
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    argv = json.loads((tmp_path / "argv.json").read_text())
    text = argv[argv.index("-p") + 1]
    assert text.startswith(cli.CONTINUATION_TEXT + "\n\n" + channel.PROMPT_HEADING)
    assert "from K (sent 869.03.01): join my war on Ludwig" in text
    rows = _turns(play)
    assert rows[1]["text"] == text and rows[1]["messages_delivered"] == [sent["msg_id"]]
    assert rows[1]["delivery"] == {"state": "delivered", "count": 1}
    row = channel.materialise(ledger.records())[0]
    assert (row["read"], row["read_via"], row["read_turn"], row["reader"]) == ("read", "resume_prompt", 1, "D1")
    # delivered once: the inbox is empty now, for observe or a later turn alike
    assert channel.deliver(channel.ChannelConfig(arm="council", topology="complete", seats=("K", "D1"),
                                                 seat="D1", quota_per_cycle=2, max_text_bytes=600),
                           ledger, game_date="869.04.03", tick=9, cycle=8) == []


def test_the_hooks_sink_variable_is_the_runs():
    """One spelling: the run sets HOOK_STATE_OUT_ENV, the hook reads it."""
    src = (ROOT / "ck3agent" / "hooks" / "inject_state.py").read_text(encoding="utf-8")
    assert f'HOOK_STATE_OUT_ENV = "{cli.HOOK_STATE_OUT_ENV}"' in src


def test_a_cell_seat_past_its_continuations_cap_is_refused(play, tmp_path, monkeypatch):
    """The cap: a seat that keeps ending early is resumed --continuations times,
    then the run exits non-zero with stop_reason continuations_exhausted (never 0,
    never `horizon`). Red on 85c6dd98: exit 0 after the first session."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setattr(cli, "DEFAULT_CONTINUATIONS", 2, raising=False)
    rc = cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "30"])
    m = manifest_of(play)
    assert rc == getattr(cli, "EXIT_CONTINUATIONS_EXHAUSTED", 3) and m["exit_code"] == rc, m
    assert m["stop_reason"] == "continuations_exhausted" and m["horizon_reached"] is False
    assert len(m["continuations"]) == 2 == m["continuations_cap"] and _resumes(tmp_path) == ["s1", "s1"]


def _agent_ids(tmp_path: Path) -> list[str]:
    """CK3_AGENT_CHARACTER_ID as each claude process saw it, one line per process."""
    p = tmp_path / "argv.json.agent_ids"
    return p.read_text().splitlines() if p.exists() else []


def test_outside_a_cell_a_session_that_ends_early_is_not_resumed(play, tmp_path, monkeypatch):
    """The behaviour control (r2 item 5): the SAME fake as the resumed test (it
    ends on its own after its result, and would reach the horizon if resumed),
    run without the cell's start: exactly one claude process, never resumed."""
    monkeypatch.delenv("CK3_EPISODE_HORIZON_START", raising=False)
    monkeypatch.setenv("CK3_EPISODE_HORIZON_END", "867.07.02")
    monkeypatch.setenv("FAKE_RESUME_HORIZON", "1")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    assert len(_agent_ids(tmp_path)) == 1 and _resumes(tmp_path) == []
    m = manifest_of(play)
    assert m["stop_reason"] is None and m["result_events"] == 1 and m["cycles_met"] is None


def test_continuations_flag_sets_the_cap(play, tmp_path, monkeypatch):
    """`--continuations N` itself (r2 item 5): N=1 resumes once, then exit 3."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    rc = cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "30",
                   "--continuations", "1"])
    m = manifest_of(play)
    assert rc == cli.EXIT_CONTINUATIONS_EXHAUSTED and len(m["continuations"]) == 1 == m["continuations_cap"]
    assert len(_agent_ids(tmp_path)) == 2 and _resumes(tmp_path) == ["s1"]


def test_a_resumed_seat_binds_the_ruler_it_held_not_the_declared_one(play, tmp_path, monkeypatch):
    """r2 item 1: the declared ruler 100 died and the seat was rebound to 300.
    The resumed claude's NEW server pre-binds CK3_AGENT_CHARACTER_ID; the run
    now carries what the old server's session held at exit (its release record),
    so the resumed server binds 300, not the dead 100. Red on b6998ec5: '100'."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "100")
    monkeypatch.setenv("FAKE_HELD", "[300]")
    monkeypatch.setenv("FAKE_RESUME_HORIZON", "1")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    assert _agent_ids(tmp_path) == ["'100'", "'300'"]
    c = manifest_of(play)["continuations"][0]
    assert c["carried_rulers"] == [300] and c["carried_from"] == "release_record"


def test_without_a_release_record_the_sink_names_the_ruler(play, tmp_path, monkeypatch):
    """r2 item 1, the fallback: no release record (a server killed at exit); the
    seat's lineage sink's last row is the rebind 100 -> 300, so 300 is carried."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    sink = tmp_path / "seat-K-100.seat_lineage.jsonl"
    sink.write_text(json.dumps({"kind": "bound", "old_id": 0, "new_id": 100}) + "\n"
                    + json.dumps({"kind": "rebind", "old_id": 100, "new_id": 300}) + "\n")
    monkeypatch.setenv("CK3_SEAT_LINEAGE_OUT", str(sink))
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "100")
    monkeypatch.setenv("FAKE_RESUME_HORIZON", "1")
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    assert _agent_ids(tmp_path) == ["'100'", "'300'"]
    assert manifest_of(play)["continuations"][0]["carried_from"] == "lineage_sink"


def test_a_claude_that_exits_before_its_horizon_reply_is_drained_is_not_resumed(play, tmp_path, monkeypatch):
    """r2 item 2: the loop tested horizon.reached BEFORE the drain had fed the
    server's horizon reply; the fake writes the reply and exits at once, and the
    drain is held back until claude has exited. Red on b6998ec5: one continuation."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("FAKE_HORIZON", "reply")
    real = cli._Drain._copy

    def slow_copy(self):
        time.sleep(1.5)                      # claude has exited; the reply is still in the pipe
        return real(self)
    monkeypatch.setattr(cli._Drain, "_copy", slow_copy)
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    m = manifest_of(play)
    assert m["continuations"] == [] and len(_agent_ids(tmp_path)) == 1, m["continuations"]
    assert m["stop_reason"] == "horizon" and m["horizon_reached"] is True


def test_a_cell_seat_that_ends_right_after_the_horizon_reply_is_not_resumed(play, tmp_path, monkeypatch):
    """The server said horizon and claude ended on its own before the watch's
    next poll: the run's stop is the horizon, exit 0, and nothing is resumed."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("FAKE_HORIZON", "reply")          # FAKE_MODE finish: exits right after the reply
    assert cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", "--wall-seconds", "20"]) == 0
    m = manifest_of(play)
    assert m["stop_reason"] == "horizon" and m["horizon_reached"] is True, m
    assert m["continuations"] == [] and _resumes(tmp_path) == []


@pytest.mark.parametrize("how", ["wall_seconds", "cap"])
def test_a_cell_run_that_stops_without_a_resume_keeps_its_rulers_record(play, tmp_path, monkeypatch, how):
    """L23 witness r3 item 1: the resume loop consumed the old server's release
    and session records BEFORE deciding not to resume, so a wall stop or an
    exhausted cap in a cell recorded held_at_stop [] (and waited 5 s). The
    records are consumed only when a resume is certain. Red on 685cb506: []."""
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("FAKE_HELD", "[300]")
    if how == "wall_seconds":
        monkeypatch.setenv("FAKE_MODE", "hang-result")
        argv = ["--wall-seconds", "3"]
    else:
        argv = ["--wall-seconds", "30", "--continuations", "0"]
    cli.main(["run", "--no-game-check", "--quiet", "--cycles", "0", *argv])
    m = manifest_of(play)
    assert m["continuations"] == [] and len(_agent_ids(tmp_path)) == 1
    assert m["rulers"]["held_at_stop"] == [300], m["rulers"]


# ── 2026.12.5: the seat's bridge session is handed over, never left blocking ──
#
# v3 pilot smoke (orders_hom, 2026-11-09): a cell seat's claude ended, the run
# resumed it, and the resumed claude's NEW server pre-bound the seat's ruler on a
# fresh session while the old session (pinned by the bridge in an experiment,
# its last claim unreleasable) still held it: owned_by_other, K unbound for 94
# game-days. Red on 5718f403: the second bind is made by a different session.

def _binds(fb) -> list:
    return [(s, p["character_id"]) for s, a, p in fb.log if a == "set_controlled_character"]


def test_a_cell_continuation_hands_the_seats_session_to_the_new_server(play, bridge, tmp_path, monkeypatch):
    """Also the seat's BUDGET identity (signed v3 §6 bounds tokens per seat per
    game-year): the bridge's budget gate is keyed by the bridge session id
    (dispatch_router.cpp, budget_ledger().evaluate(session->session_id, ...)),
    so a continuation on a fresh session restarted the seat's ceiling at 0. The
    resumed server's every request now carries the FIRST session's id."""
    fb = bridge(pin_claims=True)
    for k, v in CELL_ENV.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    monkeypatch.setenv("FAKE_RESUME_HORIZON", "1")
    assert cli.main(["run", "--quiet", "--cycles", "0", "--wall-seconds", "60"]) == 0
    m = manifest_of(play)
    log = (tmp_path / "server.log").read_text()[-3000:]
    c = m["continuations"]
    assert len(c) == 1 and m["stop_reason"] == "horizon", (c, m["stop_reason"])
    binds = _binds(fb)
    first = binds[0][0]
    # both servers bound 42, on ONE session: the second took the first's over
    assert binds == [(first, 42), (first, 42)], (binds, log)
    assert (f"tok-{first}", True, "ok") in fb.resumes, fb.resumes
    ho = c[0]["handover"]
    assert ho["session_id"] == first and ho["session_state"] == "detached" and ho["resumable"] is True
    assert ho["claims"] == [42] and c[0]["carried_rulers"] == [42]
    assert fb.owner == {42: first}
    # every session the seat's servers acted on is the first one (the budget key);
    # the CLI's own links are client "ck3agent-cli*" and spend nothing
    seat_sessions = {s for s, a, p in fb.log if fb.sessions.get(s, fb.destroyed.get(s, {}))
                     .get("client_name") == "ck3agent"}
    assert seat_sessions == {first}, seat_sessions


def _dead_run(play: Path, stamp: str = "20000101-000000", lock: bool = True, keep_open: bool = False):
    """An earlier run of this play dir that died before its teardown: its server's
    session claimed 42 and its connection is gone (the fake pins it, detached).
    Returns (session id, the open client when keep_open)."""
    sock = os.environ["CK3_BRIDGE_SOCKET"]
    cm = br.session(sock, "ck3agent", mode="control")
    bc = cm.__enter__()
    br.call(bc, "set_controlled_character", {"character_id": 42})
    sid, tok = bc.handshake_ack["session_id"], bc.handshake_ack["resume_token"]
    if not keep_open:
        cm.__exit__(None, None, None)
        time.sleep(0.2)                       # the fake marks it detached as the socket closes
    runs = play / "runs"
    runs.mkdir(exist_ok=True)
    (runs / f"{stamp}.session.json").write_text(json.dumps(
        {"session_id": sid, "resume_token": tok, "pid": 1, "written_utc": "2000-01-01T00:00:00Z"}))
    if lock:
        (runs / f"{stamp}.run.lock").touch()
    return sid, (cm if keep_open else None)


def test_a_run_takes_over_the_seat_a_dead_earlier_run_left_holding_it(play, bridge, tmp_path, monkeypatch):
    """The restart case: the stale holder's run is dead (its lock is free) and its
    session has no connection, so this run's server resumes it and binds 42 there.
    Red on 5718f403: a fresh session's bind of 42 is refused owned_by_other."""
    fb = bridge(pin_claims=True)
    sid, _ = _dead_run(play)
    monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
    monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
    monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
    assert cli.main(["run", "--quiet"]) == 0
    m = manifest_of(play)
    assert _binds(fb) == [(sid, 42), (sid, 42)], _binds(fb)
    assert m["adopted_session"]["session_id"] == sid and m["adopted_session"]["claims"] == [42]
    assert m["rulers"]["bound_at_start"] == [42] and m["rulers"]["refused_at_start"] == {}
    assert not (play / "runs" / "20000101-000000.session.json").exists()


@pytest.mark.parametrize("why", ["run_alive", "session_attached", "no_lock_file"])
def test_a_live_or_unknown_holder_is_never_taken_over(play, bridge, tmp_path, monkeypatch, why):
    """The controls: the other run still holds its lock (alive, maybe between two
    claudes), or its session is still connected, or the record predates the lock
    (cannot tell): nothing is adopted, the record stays, and the bind is the
    bridge's refusal. NON-DISCRIMINATING: green on 5718f403 too (nothing was
    ever adopted there; `adopted_session` is read with .get for that reason)."""
    import fcntl
    fb = bridge(pin_claims=True)
    sid, cm = _dead_run(play, lock=why != "no_lock_file", keep_open=why == "session_attached")
    held = None
    if why == "run_alive":
        held = os.open(play / "runs" / "20000101-000000.run.lock", os.O_WRONLY)
        fcntl.flock(held, fcntl.LOCK_EX)
    try:
        monkeypatch.setenv("FAKE_SERVE", str(tmp_path / "server.log"))
        monkeypatch.setenv("FAKE_WAIT_LOG", ARMED)
        monkeypatch.setenv("CK3_AGENT_CHARACTER_ID", "42")
        cli.main(["run", "--quiet"])
        m = manifest_of(play)
        assert m.get("adopted_session") is None
        assert m["rulers"]["refused_at_start"] == {"42": "owned_by_other"}, m["rulers"]
        assert fb.owner[42] == sid
        assert (play / "runs" / "20000101-000000.session.json").exists()
    finally:
        if held is not None:
            os.close(held)
        if cm is not None:
            cm.__exit__(None, None, None)


def _alive(pid: int) -> bool:
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().split(") ", 1)[1][0] != "Z"
    except (OSError, IndexError):
        return False


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="PR_SET_PDEATHSIG is Linux")
def test_a_killed_run_takes_its_claude_with_it(play, tmp_path, monkeypatch):
    """2026.12.5 (draft smoke): SIGKILL to `ck3agent run` left its claude
    playing ~108 s to the horizon. The claude it spawns now gets SIGTERM from the
    kernel when the run dies (the fake claude exits 143 on SIGTERM). Red on
    e8411492: the fake claude is still alive after the run is killed. Only
    processes this test started are signalled, by their own recorded pids."""
    monkeypatch.setenv("FAKE_MODE", "hang-ignore")
    pidfile = tmp_path / "argv.json.pid"
    run = subprocess.Popen([sys.executable, "-m", "ck3agent", "run", "--no-game-check", "--quiet",
                            "--cycles", "0"], stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    claude_pid = None
    try:
        for _ in range(200):
            if pidfile.exists() and pidfile.read_text().strip():
                claude_pid = int(pidfile.read_text())
                break
            time.sleep(0.05)
        assert claude_pid and _alive(claude_pid), "the fake claude never started"
        # the claude is the run's own child, with claude's argv (not the shim's)
        assert json.loads((tmp_path / "argv.json").read_text())[0] != "-c"
        run.send_signal(signal.SIGKILL)
        run.wait(10)
        gone = False
        for _ in range(100):
            if not _alive(claude_pid):
                gone = True
                break
            time.sleep(0.05)
        assert gone, f"claude {claude_pid} outlived its killed run"
    finally:
        if run.poll() is None:
            run.kill()
        if claude_pid and _alive(claude_pid):
            os.kill(claude_pid, signal.SIGKILL)


_NO_CTYPES = "import sys; sys.modules['ctypes'] = None\n"
_PRCTL_FAILS = ("import ctypes\n"
                "class _F:\n"
                "    def prctl(self, *a): return -1\n"
                "ctypes.CDLL = lambda *a, **k: _F()\n")


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="PR_SET_PDEATHSIG is Linux")
@pytest.mark.parametrize("case", ["ok", "no_ctypes_in_the_shim", "prctl_fails"])
def test_the_shim_starts_claude_and_says_when_it_cannot_tie_it_to_the_run(case):
    """r3 witness item 3. The shim ignored prctl's return (a failure fell back
    silently) and a Python without ctypes never started claude (rc 1). Now
    claude starts in every case, and a failure is one stderr line naming the
    step and the errno or exception class. Red on 5e42c229: no_ctypes (rc 1)
    and prctl_fails (silent). `ok` is the control (green on both)."""
    argv = cli.dies_with_run(["/bin/echo", "claude-started"])
    assert argv[:3] == [sys.executable, "-I", "-c"]
    pre = {"ok": "", "no_ctypes_in_the_shim": _NO_CTYPES, "prctl_fails": _PRCTL_FAILS}[case]
    argv[3] = pre + argv[3]
    r = subprocess.run(argv, capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and r.stdout.strip() == "claude-started", (r.returncode, r.stdout, r.stderr)
    said = "will NOT stop when the run dies" in r.stderr
    assert said == (case != "ok"), r.stderr
    if case != "ok":
        assert r.stderr.count("\n") == 1 and "PR_SET_PDEATHSIG" in r.stderr


def test_a_python_without_ctypes_spawns_claude_directly(monkeypatch):
    """r3 witness item 3: with no ctypes the run does not wrap claude at all,
    and the manifest's `claude_dies_with_run` says why."""
    import importlib.util
    real = importlib.util.find_spec
    monkeypatch.setattr(importlib.util, "find_spec",
                        lambda name, *a, **k: None if name == "ctypes" else real(name, *a, **k))
    argv = ["/bin/echo", "x"]
    if sys.platform.startswith("linux"):
        assert cli.pdeathsig_shim_available() == (False, "this Python has no ctypes")
    assert cli.dies_with_run(argv) == argv
