"""ck3agent — let Claude play Crusader Kings III through an MCP server.

The package has two halves:

* ``ck3agent.server`` — the MCP server (``mcp_server.py``) and the bridge
  client it uses to talk to ``libck3_bridge.so`` over a Unix socket. Those
  modules are shared verbatim with the research checkout and use bare sibling
  imports, so they are loaded through :func:`server_dir` rather than as a
  regular subpackage.
* the CLI (``ck3agent.cli``): ``setup``, ``doctor``, ``launch``, ``serve``,
  ``run``.
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
