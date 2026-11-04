"""Configuration and install discovery.

Files (XDG; an empty variable counts as unset):

* config: ``$XDG_CONFIG_HOME/ck3agent/config.toml`` (``CK3AGENT_CONFIG`` overrides)
* data:   ``$XDG_DATA_HOME/ck3agent/`` (``lib/libck3_bridge.so``)
* game:   ``$XDG_STATE_HOME/ck3agent/game/``: the user-data directory of the
  game ``ck3agent launch`` starts (its saves, autosaves, logs, continue_game.json)

Each setting resolves in one order: flag, environment, config file, discovery.
:func:`resolve` is the only place that order lives.
"""
from __future__ import annotations

import datetime as _dt
import os
import re
import tomllib
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import NamedTuple

STEAM_APPID = "1158310"
CK3_REL = Path("steamapps/common/Crusader Kings III/binaries/ck3")


def _env(name: str) -> str | None:
    v = os.environ.get(name)
    return v if v else None


def config_dir() -> Path:
    return Path(_env("XDG_CONFIG_HOME") or Path.home() / ".config") / "ck3agent"


def data_dir() -> Path:
    return Path(_env("XDG_DATA_HOME") or Path.home() / ".local/share") / "ck3agent"


def config_path() -> Path:
    return Path(_env("CK3AGENT_CONFIG") or config_dir() / "config.toml")


def state_dir() -> Path:
    return Path(_env("XDG_STATE_HOME") or Path.home() / ".local/state") / "ck3agent"


def owner_game_dir() -> Path:
    """The user's own CK3 user-data tree, where the game writes when the user plays.
    CK3 picks ``$HOME/.local/share`` whatever XDG says. ck3agent only reads it."""
    return Path.home() / ".local/share/Paradox Interactive/Crusader Kings III"


def game_user_dir() -> Path:
    """The user-data tree of the game ``ck3agent launch`` starts (``-userdir=``):
    its saves, autosaves, continue_game.json and logs land here, never in
    :func:`owner_game_dir`."""
    return state_dir() / "game"


def game_debug_log() -> Path:
    return game_user_dir() / "logs/debug.log"


@dataclass
class Config:
    ck3_binary: str = ""
    bridge_so: str = ""
    play_dir: str = ""
    extra: dict = field(default_factory=dict)

    @classmethod
    def load(cls) -> "Config":
        p = config_path()
        if not p.exists():
            return cls()
        with p.open("rb") as f:
            d = tomllib.load(f)
        known = {k: str(d.pop(k)) for k in list(d) if k in cls.__dataclass_fields__ and k != "extra"}
        return cls(**known, extra=d)

    def save(self) -> Path:
        p = config_path()
        p.parent.mkdir(parents=True, exist_ok=True)
        lines = ["# ck3agent configuration (written by `ck3agent setup`; edit freely)"]
        tables = []
        for k, v in asdict(self).items():
            if k != "extra":
                lines.append(f"{_key(k)} = {_scalar(str(v))}")
        for k, v in self.extra.items():
            if isinstance(v, dict):
                tables.append((k, v))
            else:
                lines.append(f"{_key(k)} = {_scalar(v)}")
        for k, t in tables:
            lines += ["", f"[{_key(k)}]"]
            lines += [f"{_key(sk)} = {_scalar(sv)}" for sk, sv in t.items()]
        p.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return p


_BARE = re.compile(r"^[A-Za-z0-9_-]+$")
_SHORT = {"\b": "\\b", "\t": "\\t", "\n": "\\n", "\f": "\\f", "\r": "\\r", '"': '\\"', "\\": "\\\\"}


def _esc(s: str) -> str:
    out = []
    for ch in s:
        if ch in _SHORT:
            out.append(_SHORT[ch])
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append(f"\\u{ord(ch):04x}")
        else:
            out.append(ch)
    return "".join(out)


def _key(k: str) -> str:
    return k if _BARE.match(k) else f'"{_esc(k)}"'


def _scalar(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        if v != v:
            return "nan"
        if v in (float("inf"), float("-inf")):
            return "inf" if v > 0 else "-inf"
        return repr(v)
    if isinstance(v, (_dt.datetime, _dt.date, _dt.time)):
        return v.isoformat()
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(_scalar(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ", ".join(f"{_key(k)} = {_scalar(x)}" for k, x in v.items()) + "}"
    return f'"{_esc(str(v))}"'


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
    """Steam discovery only. Use :func:`resolve` for the full precedence."""
    for lib in steam_libraries():
        p = lib / CK3_REL
        if p.is_file():
            return p
    return None


def find_bridge_so() -> Path | None:
    p = data_dir() / "lib/libck3_bridge.so"
    return p if p.is_file() else None


# ── one precedence for every setting ─────────────────────────────────────────

class Resolved(NamedTuple):
    path: Path | None
    source: str        # flag | env NAME | config | discovery | default | unset


DEFAULT_PLAY_DIR_NAME = "ck3agent-play"

#: setting -> (environment variable or None, discovery function)
SETTINGS = {
    "ck3_binary": ("CK3_BINARY", find_ck3_binary),
    "bridge_so": ("CK3AGENT_BRIDGE", find_bridge_so),
    "play_dir": (None, None),
}


def resolve(name: str, c: "Config | None" = None, flag: str | None = None) -> Resolved:
    env_name, discover = SETTINGS[name]
    if flag:
        return Resolved(Path(flag).expanduser(), "flag")
    if env_name and _env(env_name):
        return Resolved(Path(_env(env_name)).expanduser(), f"env {env_name}")
    c = c if c is not None else Config.load()
    val = getattr(c, name, "")
    if val:
        return Resolved(Path(val).expanduser(), "config")
    if name == "play_dir":
        return Resolved(Path.home() / DEFAULT_PLAY_DIR_NAME, "default")
    p = discover() if discover else None
    return Resolved(p, "discovery" if p else "unset")


def game_version_from_install(ck3_binary: Path) -> str | None:
    """CK3's own version label, from launcher/launcher-settings.json next to the binary."""
    settings = ck3_binary.parent.parent / "launcher/launcher-settings.json"
    if not settings.exists():
        return None
    m = re.search(r'"rawVersion"\s*:\s*"([^"]+)"', settings.read_text(errors="replace"))
    return m.group(1) if m else None
