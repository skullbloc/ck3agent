"""ck3agent: let Claude play Crusader Kings III through an MCP server.

* ``ck3agent/server``: the MCP server and its bridge client. These modules are
  shared verbatim with the research checkout and use bare sibling imports, so
  they load through :func:`bootstrap_server`, not as a subpackage.
* the CLI: ``cli.py`` (commands), ``_bridge.py`` (every server-module import),
  ``_config.py``, ``_console.py``, ``_elf.py``.
"""
from __future__ import annotations

import sys
from pathlib import Path

from ._version import __version__

__all__ = ["__version__", "server_dir", "bootstrap_server"]


def server_dir() -> Path:
    return Path(__file__).resolve().parent / "server"


def bootstrap_server() -> Path:
    """Put the server directory first on sys.path so its bare imports resolve."""
    d = str(server_dir())
    if d not in sys.path:
        sys.path.insert(0, d)
    return Path(d)
