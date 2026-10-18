"""User configuration and install discovery for the ck3agent CLI.

Locations (XDG, with the usual fallbacks):

* config:  ``$XDG_CONFIG_HOME/ck3agent/config.toml``  (default ``~/.config/ck3agent``)
* data:    ``$XDG_DATA_HOME/ck3agent/``                (default ``~/.local/share/ck3agent``)
           ``lib/libck3_bridge.so`` and ``share/trigger_subclasses.csv`` live here
           after ``install.sh``.

Every lookup can be overridden by environment: ``CK3_BINARY``,
``CK3AGENT_BRIDGE`` (path to the .so), ``CK3AGENT_CONFIG`` (path to the toml).
"""
from __future__ import annotations

import os
import re
import tomllib
from dataclasses import dataclass, field, asdict
from pathlib import Path

STEAM_APPID = "1158310"
CK3_REL = Path("steamapps/common/Crusader Kings III/binaries/ck3")


def config_dir() -> Path:
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "ck3agent"


def data_dir() -> Path:
    return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "ck3agent"


def config_path() -> Path:
    return Path(os.environ.get("CK3AGENT_CONFIG", config_dir() / "config.toml"))


@dataclass
class Config:
    ck3_binary: str = ""
    bridge_so: str = ""
    trigger_csv: str = ""
    play_dir: str = ""
    extra: dict = field(default_factory=dict)

    @classmethod
    def load(cls) -> "Config":
        p = config_path()
        if not p.exists():
            return cls()
        with p.open("rb") as f:
            d = tomllib.load(f)
        known = {k: d.pop(k) for k in list(d) if k in cls.__dataclass_fields__ and k != "extra"}
        return cls(**known, extra=d)

    def save(self) -> Path:
        p = config_path()
        p.parent.mkdir(parents=True, exist_ok=True)
        lines = ["# ck3agent configuration (written by `ck3agent setup`; edit freely)"]
        for k, v in asdict(self).items():
            if k == "extra":
                continue
            lines.append(f'{k} = "{_esc(str(v))}"')
        for k, v in self.extra.items():
            lines.append(f'{k} = "{_esc(str(v))}"')
        p.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return p


def _esc(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


# ── discovery ────────────────────────────────────────────────────────────────

def steam_roots() -> list[Path]:
    home = Path.home()
    cands = [
        home / ".steam/steam",
        home / ".local/share/Steam",
        home / ".var/app/com.valvesoftware.Steam/.local/share/Steam",   # flatpak
        home / ".steam/root",
    ]
    out: list[Path] = []
    for c in cands:
        try:
            r = c.resolve()
        except OSError:
            continue
        if r.is_dir() and r not in out:
            out.append(r)
    return out


def steam_libraries() -> list[Path]:
    """Every Steam library folder, from libraryfolders.vdf plus the roots."""
    libs: list[Path] = []
    for root in steam_roots():
        if root not in libs:
            libs.append(root)
        vdf = root / "steamapps/libraryfolders.vdf"
        if vdf.exists():
            for m in re.finditer(r'"path"\s+"([^"]+)"', vdf.read_text(errors="replace")):
                p = Path(m.group(1).replace("\\\\", "\\"))
                if p.is_dir() and p not in libs:
                    libs.append(p)
    return libs


def find_ck3_binary() -> Path | None:
    env = os.environ.get("CK3_BINARY")
    if env:
        return Path(env)
    for lib in steam_libraries():
        p = lib / CK3_REL
        if p.is_file():
            return p
    return None


def find_bridge_so() -> Path | None:
    env = os.environ.get("CK3AGENT_BRIDGE")
    if env:
        return Path(env)
    p = data_dir() / "lib/libck3_bridge.so"
    return p if p.is_file() else None


def find_trigger_csv() -> Path | None:
    p = data_dir() / "share/trigger_subclasses.csv"
    return p if p.is_file() else None


def game_version_from_install(ck3_binary: Path) -> str | None:
    """CK3's own version label, from launcher/launcher-settings.json next to the binary."""
    settings = ck3_binary.parent.parent / "launcher/launcher-settings.json"
    if not settings.exists():
        return None
    m = re.search(r'"rawVersion"\s*:\s*"([^"]+)"', settings.read_text(errors="replace"))
    return m.group(1) if m else None
