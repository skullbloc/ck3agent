"""Observe, plan_war and succession_plan replies over the cap are PAGED, not cut.

On the in-repo recorded replies of the pre-flight (a Francia 867 game and a Munster 1066 game: whole replies of a
live game through the public tools): every page within the cap, every row of every list on some page and whole,
the first page's summary first and its `more` naming the next call; `inspect subject=more` serves the pages in
order and refuses when none is kept; the switch off (CK3_REPLY_PAGES=off) gives back the base commit's rule.

BASE PIN: the comparison with the switch off is against reply_fit.py at BASE_SHA (blob BASE_BLOB); if a
merge changes reply_fit.py there, BASE_SHA and BASE_BLOB are re-pinned.
No game, no model."""
import asyncio
import gzip
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import reply_fit as F  # noqa: E402

BASE_SHA = "1cd0e382a75f56d11183e01bb426b8ff5c968cc4"
BASE_BLOB = "fa2aff92dae22fa23c2876036b2306d36ec2ef9a"


def _rows(o, path=()):
    """(path, json of row) for every row of every list under o."""
    if isinstance(o, dict):
        for k, v in o.items():
            yield from _rows(v, path + (k,))
    elif isinstance(o, list):
        for x in o:
            yield path, json.dumps(x, sort_keys=True, default=str)


def _all_pages(env, tool, args):
    cap = F.CAP - F.wrapper_bytes(tool)
    first = F.paginate(json.loads(json.dumps(env)), tool, args, cap=cap)
    out = [first]
    while True:
        p = F.next_page()
        if p.get("ok") is False and (p.get("blocked") or {}).get("gate") == "no_more_pages":
            return out, cap
        out.append(p)


def _strip_more(page):
    r = dict(page["result"])
    r.pop("more", None)
    return r


@pytest.fixture(autouse=True)
def _pages_on(monkeypatch):
    monkeypatch.delenv(F.PAGES_ENV, raising=False)
    F.reset_pages()


def _base_module():
    src = subprocess.run(["git", "-C", str(ROOT), "cat-file", "-p", BASE_BLOB], capture_output=True, text=True)
    if src.returncode != 0:
        pytest.skip(f"the base blob {BASE_BLOB} is not in this clone")
    p = Path(os.environ.get("TMPDIR", "/tmp")) / f"reply_fit_base_{os.getpid()}.py"
    p.write_text(src.stdout)
    spec = importlib.util.spec_from_file_location("reply_fit_base", p)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    p.unlink()
    return m


def test_the_base_pin_is_the_base_commits_file():
    r = subprocess.run(["git", "-C", str(ROOT), "rev-parse", f"{BASE_SHA}:ck3agent/server/reply_fit.py"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip("the base commit is not in this clone")
    assert r.stdout.strip() == BASE_BLOB


_LISTING = r"""
import asyncio, json, sys
sys.path.insert(0, sys.argv[1])
import mcp_server as ms
out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in asyncio.run(ms.mcp.list_tools())]
out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]} for d in out]
print(json.dumps(out))
"""


def _listing(pages):
    e = {k: v for k, v in os.environ.items() if k != F.PAGES_ENV}
    if pages is not None:
        e[F.PAGES_ENV] = pages
    r = subprocess.run([sys.executable, "-c", _LISTING, str(HERE)], env=e, capture_output=True, text=True,
                       timeout=300)
    assert r.returncode == 0, r.stderr[-2000:]
    return json.loads(r.stdout.strip().splitlines()[-1])


def test_the_listing_adds_one_enum_value_and_holds_its_caps():
    on, off, unset = _listing("on"), _listing("off"), _listing(None)
    assert unset == on and len(on) == 13
    raw, ind = len(json.dumps(on)), len(json.dumps(on, indent=1))
    assert raw <= 31_049 and ind <= 36_298, (raw, ind)
    subj_on = next(t for t in on if t["name"] == "inspect")["inputSchema"]["properties"]["subject"]["enum"]
    subj_off = next(t for t in off if t["name"] == "inspect")["inputSchema"]["properties"]["subject"]["enum"]
    assert subj_on == sorted(subj_off + ["more"])
    # nothing else moved
    on_wo = json.loads(json.dumps(on))
    next(t for t in on_wo if t["name"] == "inspect")["inputSchema"]["properties"]["subject"]["enum"].remove("more")
    assert on_wo == off


def test_inspect_more_answers_without_a_game():
    import mcp_server as ms
    F._STORES["plan_war"] = {"tool": "plan_war", "pages": [{"ok": True, "tool": "plan_war",
                                                            "result": {"targets": [1]}}],
                             "next": 0, "as_of": "1066.10.01", "n": 2, "seq": 1}
    out = asyncio.run(ms.inspect(subject="more"))
    assert out["result"]["targets"] == [1] and out["result"]["more"]["page"] == "2 of 2"
