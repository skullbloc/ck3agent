"""CLI tests that need no game and no bridge binary."""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from ck3agent import __version__, cli, _config as cfg
from ck3agent._bridge_meta import SUPPORTED_BUILD_ID, SUPPORTED_GAME_VERSION
from ck3agent._elf import read_build_id


def test_version_matches_stamped_files():
    root = Path(__file__).resolve().parents[1]
    assert (root / "VERSION").read_text().strip() == __version__


def test_supported_build_is_a_real_build_id():
    assert len(SUPPORTED_BUILD_ID) == 40 and int(SUPPORTED_BUILD_ID, 16)
    assert SUPPORTED_GAME_VERSION[0].isdigit()


def test_read_build_id_matches_readelf_on_python_binary():
    exe = Path(sys.executable).resolve()
    try:
        out = subprocess.run(["readelf", "-n", str(exe)], capture_output=True, text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        pytest.skip("readelf unavailable")
    import re
    m = re.search(r"Build ID:\s*([0-9a-f]+)", out)
    if not m:
        pytest.skip("python binary carries no build-id")
    assert read_build_id(exe) == m.group(1)


def test_setup_writes_play_dir_and_config(tmp_path, monkeypatch):
    home = tmp_path / "home"; home.mkdir()
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("XDG_CONFIG_HOME", str(home / ".config"))
    monkeypatch.setenv("XDG_DATA_HOME", str(home / ".local/share"))
    monkeypatch.delenv("CK3_BINARY", raising=False)
    # a fake install: the python binary stands in for ck3 (an ELF the BuildID reader can parse)
    fake_ck3 = home / "steam/steamapps/common/Crusader Kings III/binaries/ck3"
    fake_ck3.parent.mkdir(parents=True)
    fake_ck3.write_bytes(Path(sys.executable).resolve().read_bytes())
    fake_ck3.chmod(0o755)
    so = home / ".local/share/ck3agent/lib/libck3_bridge.so"
    so.parent.mkdir(parents=True); so.write_bytes(b"\x7fELF")
    play = tmp_path / "play"
    rc = cli.main(["setup", "--ck3", str(fake_ck3), "--play-dir", str(play)])
    assert rc == 0
    mcp = json.loads((play / ".mcp.json").read_text())
    assert mcp["mcpServers"]["ck3"]["args"] == ["serve"]
    assert (play / "CLAUDE.md").read_text().startswith("# CLAUDE.md")
    hooks = json.loads((play / ".claude/settings.json").read_text())
    assert "UserPromptSubmit" in hooks["hooks"]
    c = cfg.Config.load()
    assert c.ck3_binary == str(fake_ck3) and c.bridge_so == str(so) and c.play_dir == str(play)


def test_doctor_reports_unsupported_build_and_no_game(tmp_path, monkeypatch, capsys):
    home = tmp_path / "home"; home.mkdir()
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("XDG_CONFIG_HOME", str(home / ".config"))
    monkeypatch.setenv("XDG_DATA_HOME", str(home / ".local/share"))
    monkeypatch.setenv("CK3_BINARY", sys.executable)          # a real ELF with the WRONG build id
    monkeypatch.setenv("CK3_BRIDGE_SOCKET", str(tmp_path / "nope.sock"))
    monkeypatch.setenv("CK3_BRIDGE_LOG", str(tmp_path / "nope.log"))
    rc = cli.main(["doctor"])
    out = capsys.readouterr().out
    assert rc == 1
    assert "NOT supported" in out
    assert "bridge socket not listening" in out


def test_doctor_surfaces_bridge_refusal_from_log(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("CK3_BRIDGE_SOCKET", str(tmp_path / "nope.sock"))
    log = tmp_path / "bridge.log"
    log.write_text("[CK3-Bridge] REFUSED: unsupported CK3 build.\n[CK3-Bridge]   the running binary has BuildID deadbeef\n")
    monkeypatch.setenv("CK3_BRIDGE_LOG", str(log))
    cli.main(["doctor"])
    out = capsys.readouterr().out
    assert "bridge refused to load" in out and "deadbeef" in out


def test_serve_help_and_version(capsys):
    with pytest.raises(SystemExit) as e:
        cli.main(["serve", "--help"])
    assert e.value.code == 0
    assert cli.main(["version"]) == 0
    assert __version__ in capsys.readouterr().out


def test_mcp_server_imports_and_registers_tools():
    from ck3agent import bootstrap_server
    bootstrap_server()
    import mcp_server  # noqa: F401
    assert mcp_server.mcp is not None
