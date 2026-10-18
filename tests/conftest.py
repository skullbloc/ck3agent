"""Every test gets its own HOME, XDG dirs and bridge paths; nothing reads the real machine."""
from __future__ import annotations

import pytest

_CLEAR = ["CK3_BINARY", "CK3AGENT_BRIDGE", "CK3AGENT_CONFIG", "CK3_TRIGGER_CATALOGUE_CSV",
          "CK3_INSTANCE_ID", "CK3_INSTANCE_ROOT", "CK3_CONTROL_DIR", "WAYLAND_DISPLAY",
          "SDL_VIDEODRIVER", "LD_PRELOAD", "NO_COLOR", "CK3_TOOL_SURFACE_OUT",
          "CK3_RELEASE_ON_EXIT", "CK3_SESSION_OUT", "CK3_AGENT_CHARACTER_ID"]


@pytest.fixture(autouse=True)
def hermetic(tmp_path, monkeypatch):
    home = tmp_path / "home"
    home.mkdir()
    for k in _CLEAR:
        monkeypatch.delenv(k, raising=False)
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("XDG_CONFIG_HOME", str(home / ".config"))
    monkeypatch.setenv("XDG_DATA_HOME", str(home / ".local/share"))
    monkeypatch.setenv("XDG_RUNTIME_DIR", str(tmp_path / "run"))
    monkeypatch.setenv("CK3_BRIDGE_SOCKET", str(tmp_path / "nope.sock"))
    monkeypatch.setenv("CK3_BRIDGE_LOG", str(tmp_path / "bridge.log"))
    monkeypatch.setenv("CK3_TELE", str(tmp_path / "tele"))
    return home


@pytest.fixture
def bridge(monkeypatch):
    """start(**kw) -> a _fakes.FakeBridge on a short socket path set as CK3_BRIDGE_SOCKET."""
    import shutil
    import tempfile
    from pathlib import Path

    from _fakes import FakeBridge
    d = tempfile.mkdtemp(prefix="ck3t", dir="/tmp" if Path("/tmp").is_dir() else None)
    path = str(Path(d) / "s")
    monkeypatch.setenv("CK3_BRIDGE_SOCKET", path)
    made = []

    def start(**kw):
        fb = FakeBridge(path, **kw)
        fb.start()
        made.append(fb)
        return fb

    yield start
    for fb in made:
        fb.close()
    shutil.rmtree(d, ignore_errors=True)
