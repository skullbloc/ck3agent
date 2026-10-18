"""Status lines for the CLI. Colour only on a TTY, never with NO_COLOR set."""
from __future__ import annotations

import contextlib
import os
import sys

_CODES = {"ok": "32", "warn": "33", "FAIL": "31"}


def _color() -> bool:
    if os.environ.get("NO_COLOR") or os.environ.get("TERM") == "dumb":
        return False
    try:
        return sys.stdout.isatty()
    except (OSError, ValueError):
        return False


def _silence(stream) -> None:
    """Point the stream's fd at /dev/null: a hung-up terminal answers every write
    with EIO, including the flush at interpreter exit, which would turn the exit
    code into 120."""
    with contextlib.suppress(OSError, ValueError, AttributeError):
        null = os.open(os.devnull, os.O_WRONLY)
        try:
            os.dup2(null, stream.fileno())
        finally:
            os.close(null)
    with contextlib.suppress(OSError, ValueError):
        stream.flush()


def say(text: str = "", err: bool = False) -> None:
    """print() that survives a dead terminal: the first failed write silences the stream."""
    stream = sys.stderr if err else sys.stdout
    try:
        print(text, file=stream, flush=True)
    except OSError:
        _silence(stream)


def _line(tag: str, label: str, detail: str) -> None:
    t = f"[{tag:^4}]"
    if _color():
        t = f"\033[{_CODES[tag]}m{t}\033[0m"
    say(f"  {t} {label}" + (f": {detail}" if detail else ""))


def ok(label: str, detail: str = "") -> None:
    _line("ok", label, detail)


def warn(label: str, detail: str = "") -> None:
    _line("warn", label, detail)


def fail(label: str, detail: str = "") -> None:
    _line("FAIL", label, detail)


def note(text: str) -> None:
    say(f"         {text}")
