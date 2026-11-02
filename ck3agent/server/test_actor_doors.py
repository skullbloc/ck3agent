"""actor_doors is checked against the bridge source (L14 FF1 m-20).

Every bridge action the query and discover tools reach is classified: an actor
door (ACTOR_DOORS: its handler falls back to the bound ruler when no actor is
named), an optional one (ACTOR_OPTIONAL), or neither. The census walks
action_registry.inc to each handler and follows the calls that pass the params
on; fail OPEN: an unroutable action or an unreadable handler reds.
"""
import asyncio
import os
import re

import pytest

import actor_doors as AD
import redesign as R
import tools

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC = os.path.join(ROOT, "bridge", "src")
FALLBACK = re.compile(r"\b(resolve_character|resolve_actor|get_controlled_character)\s*\("
                      r"|\bg_controlled_primary\b")


def _sources() -> dict:
    out = {}
    for d, _, files in os.walk(SRC):
        for f in files:
            if f.endswith(".cpp"):
                with open(os.path.join(d, f), encoding="utf-8", errors="replace") as fh:
                    out[os.path.join(d, f)] = fh.read()
    assert len(out) >= 20, sorted(out)
    return out


SOURCES = _sources() if os.path.isdir(SRC) else {}
pytestmark = pytest.mark.skipif(not SOURCES, reason="bridge/src is not in this tree")


def _routes() -> dict:
    with open(os.path.join(SRC, "action_registry.inc"), encoding="utf-8") as f:
        reg = f.read()
    routes = dict(re.findall(r'^\s*BRIDGE_ACTION\("([^"]+)",\s*(\w+)\)', reg, re.M))
    assert len(routes) >= 100, len(routes)
    return routes


def _body(fn: str):
    """(body, name of its const char* params argument) of fn's definition."""
    rx = re.compile(r"^[ \t]*[A-Za-z_][^\n;{}()=]*?\b" + re.escape(fn)
                    + r"\s*\(([^)]*)\)\s*(?:const\s*)?\{", re.M)
    for text in SOURCES.values():
        m = rx.search(text)
        if not m:
            continue
        i, depth = m.end(), 1
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        nm = re.search(r"const\s+char\s*\*\s*(\w+)", m.group(1).split(",")[0])
        return text[m.end():i], (nm.group(1) if nm else "")
    return None, None


def _falls_back(fn: str, seen=None, depth=0) -> bool:
    seen = set() if seen is None else seen
    if fn in seen or depth > 5:
        return False
    seen.add(fn)
    body, pname = _body(fn)
    if body is None:
        return False
    if FALLBACK.search(body):
        return True
    if not pname:
        return False
    callees = set(re.findall(r"\b(\w+)\s*\(\s*" + pname + r"\b", body)) - {fn}
    return any(_falls_back(c, seen, depth + 1) for c in sorted(callees))


def _reached() -> dict:
    """bridge action -> the param keys the server sends it with, for every query
    domain and resolved discover subject."""
    out = {}
    for _d, (action, build) in tools.QUERY_DOMAIN_MAP.items():
        out[action] = set(build({}))
    for what in tools._DISCOVER_RESOLVED:
        action, params = tools.resolve_discover({"what": what})
        out[action] = set(params)
    return out


def test_every_reached_door_is_routed_and_its_handler_found():
    routes = _routes()
    missing = {a: routes.get(a) for a in _reached()
               if a not in routes or _body(routes[a])[0] is None}
    assert not missing, missing


def test_the_table_names_only_reached_doors():
    reached = set(_reached())
    assert set(AD.ACTOR_DOORS) <= reached, set(AD.ACTOR_DOORS) - reached
    assert set(AD.ACTOR_OPTIONAL) <= reached
    assert not set(AD.ACTOR_DOORS) & set(AD.ACTOR_OPTIONAL)
    assert set(AD.ACTOR_DOORS_VIA) <= set(AD.ACTOR_DOORS)


def test_a_door_whose_handler_falls_back_to_the_ruler_is_classified():
    """A reached handler that resolves an unnamed actor to the controlled
    character is an actor door or a declared optional one, never unclassified."""
    routes = _routes()
    loose = sorted(a for a in _reached() if _falls_back(routes[a])
                   and a not in AD.ACTOR_DOORS and a not in AD.ACTOR_OPTIONAL)
    assert not loose, loose


def test_an_actor_door_falls_back_to_the_ruler_in_the_source():
    routes = _routes()
    bad = []
    for a in list(AD.ACTOR_DOORS) + list(AD.ACTOR_OPTIONAL):
        fn = AD.ACTOR_DOORS_VIA.get(a) or routes[a]
        if not _falls_back(fn):
            bad.append((a, fn))
    assert not bad, bad


def test_an_alternative_subject_is_a_key_the_server_sends():
    reached = _reached()
    bad = {a: alt for a, alt in AD.ACTOR_DOORS.items()
           if a != "query_map_graph" and not set(alt) <= reached[a]}
    assert not bad, bad


def test_the_proxy_case_list_decisions_is_not_about_the_ruler():
    """The witness's case: discover(decisions) sends a character_id its handler
    ignores (case_list_decisions), so it is a catalog like discover(interactions)."""
    assert not AD.about_the_bound_ruler("list_decisions", {"character_id": 0})
    assert not AD.about_the_bound_ruler("list_interactions", {})
    assert AD.about_the_bound_ruler("query_realm", {"character_id": 0})
    assert not AD.about_the_bound_ruler("query_realm", {"character_id": 7})
    assert not AD.about_the_bound_ruler("query_de_jure", {"character_id": 0, "title_id": 5})
    assert AD.about_the_bound_ruler("query_de_jure", {"character_id": 0, "title_id": 0})


def test_unbound_refusal_reads_the_table(monkeypatch):
    async def refuse(_gw, _cid, tool):
        return None, {"ok": False, "blocked": {"gate": "no_ruler_bound"}, "tool": tool}
    monkeypatch.setattr(R, "_resolve_actor", refuse)
    run = asyncio.run
    assert run(R.unbound_refusal(None, "discover", "list_decisions", {"character_id": 0})) is None
    assert run(R.unbound_refusal(None, "query", "query_realm", {"character_id": 0}))["ok"] is False
