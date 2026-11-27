"""The shared notes (notes.py, notes.json): the file, its schema and limits, the
version, the off switch, and how a seat reaches them through discover.

Rows:
  * the shipped file passes notes.problems, with 20 to 35 notes, every topic within
    TOPIC_REPLY_MAX rendered and all within TOTAL_MAX;
  * each schema rule reds on a copy of the file broken in that one way (a missing
    field, an unknown kind, an unknown topic, a long text, a private string, a
    repeated id, an oversized topic), so a rule that never fires is not counted;
  * the version moves with any field of any note and not with key order;
  * the switch: unset and `on` serve, `off` serves nothing, anything else refuses;
  * the registered surface: discover's `what` enum carries `notes`, `topic`
    enumerates the file's topics, the instructions carry the index line; with
    CK3_NOTES=off (a fresh interpreter) none of the three, and a bad value exits 3;
  * discover(what='notes') answers the index and a topic with NO bridge connected,
    refuses an unknown topic by name, and session(action='health') carries the version.
"""

import asyncio
import copy
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import notes as N  # noqa: E402

DOC = json.loads(N.NOTES_FILE.read_text(encoding="utf-8"))
#: the default set, as a server with CK3_NOTES_SET unset serves it
SERVED = N.derive(DOC, None)


def run(coro):
    return asyncio.run(coro)


# ── the file ─────────────────────────────────────────────────────────────────

def test_the_shipped_file_passes_its_schema():
    assert N.problems(DOC) == []
    assert 20 <= len(DOC["notes"]) <= 35
    total = sum(len(N.render(n).encode()) for n in DOC["notes"])
    assert total <= N.TOTAL_MAX
    for t in DOC["topics"]:
        body = json.dumps([N.render(n) for n in DOC["notes"] if n["topic"] == t], ensure_ascii=False)
        assert 0 < len(body.encode()) <= N.TOPIC_REPLY_MAX, t


def _broken(fn):
    d = copy.deepcopy(DOC)
    fn(d)
    return N.problems(d)


@pytest.mark.parametrize("name,fn,needle", [
    ("missing field", lambda d: d["notes"][0].pop("provenance"), "fields"),
    ("unknown kind", lambda d: d["notes"][0].update(kind="rumour"), "kind"),
    ("unknown topic", lambda d: d["notes"][0].update(topic="weather"), "not in topics"),
    ("long text", lambda d: d["notes"][0].update(text="x" * (N.TEXT_MAX + 1)), "text is"),
    ("host path", lambda d: d["notes"][0].update(provenance="read at /home/someone/run"), "private"),
    ("tracking id", lambda d: d["notes"][1].update(provenance="see " + "ck-" + "ab12"), "private"),
    ("machine name", lambda d: d["notes"][1].update(provenance="the run on the " + "node"), "private"),
    ("e-mail", lambda d: d["notes"][1].update(text="write to a@b.c"), "private"),
    ("repeated id", lambda d: d["notes"][1].update(id=d["notes"][0]["id"]), "repeated"),
    ("bad date", lambda d: d["notes"][1].update(added="20 Nov"), "added"),
    ("line break", lambda d: d["notes"][1].update(text="one\ntwo"), "line break"),
    ("empty topic", lambda d: d["topics"].update(spare="nothing here"), "has no note"),
    ("oversized topic", lambda d: d["notes"].extend(
        dict(d["notes"][0], id=f"pad-{i}", text="y" * 200) for i in range(8)), "rendered"),
])
def test_each_schema_rule_fires_on_its_own_breakage(name, fn, needle):
    bad = _broken(fn)
    assert any(needle in p for p in bad), (name, bad)


def test_the_version_moves_with_content_and_not_with_key_order():
    v = N.version(DOC)
    assert len(v) == 12
    reordered = {"notes": [dict(reversed(list(n.items()))) for n in DOC["notes"]],
                 "topics": DOC["topics"], "format": DOC["format"], "turn_one": DOC["turn_one"]}
    assert N.version(reordered) == v
    for f in N.FIELDS:
        d = copy.deepcopy(DOC)
        d["notes"][-1][f] = d["notes"][-1][f] + "x"
        assert N.version(d) != v, f


# ── the switch ───────────────────────────────────────────────────────────────

def test_the_switch():
    on = N.boot({})
    assert on.enabled and on.version == N.version(SERVED) and on.topics == list(DOC["topics"])
    assert N.boot({"CK3_NOTES": "on"}).enabled
    off = N.boot({"CK3_NOTES": "off"})
    assert not off.enabled and off.topics == [] and off.instructions_line() == ""
    assert off.status() == {"state": "off", "switch": "CK3_NOTES=off"}
    with pytest.raises(N.NotesBootRefusal) as e:
        N.boot({"CK3_NOTES": "no"})
    assert e.value.code == "notes_switch_invalid"


def test_an_invalid_file_refuses_the_boot(tmp_path):
    p = tmp_path / "notes.json"
    p.write_text(json.dumps(dict(DOC, notes=[dict(DOC["notes"][0], kind="rumour")])))
    with pytest.raises(N.NotesBootRefusal) as e:
        N.boot({}, p)
    assert e.value.code == "notes_invalid"
    assert N.boot({"CK3_NOTES": "off"}, p).enabled is False     # off reads no file


def test_the_index_line_and_replies_are_small():
    nb = N.boot({})
    line = nb.instructions_line()
    assert nb.version in line and all(t in line for t in DOC["topics"])
    assert len(line.encode()) <= N.INDEX_LINE_MAX
    assert len(json.dumps(nb.index_reply()).encode()) <= 600
    assert nb.topic_reply("weather") is None


# ── the surface ──────────────────────────────────────────────────────────────

ms = pytest.importorskip("mcp_server")


def _discover_schema():
    (d,) = [t for t in ms.registered_tool_definitions() if t["name"] == "discover"]
    return d["input_schema"]["properties"]


def test_the_registered_surface_points_at_the_notes():
    props = _discover_schema()
    assert "notes" in props["what"]["enum"]
    assert props["topic"]["enum"] == [""] + list(DOC["topics"])
    assert N.boot({}).instructions_line() in ms.mcp.instructions


def _probe(env: dict) -> subprocess.CompletedProcess:
    code = ("import json, mcp_server as m; "
            "d = [t for t in m.registered_tool_definitions() if t['name'] == 'discover'][0]; "
            "p = d['input_schema']['properties']; "
            "print(json.dumps({'notes_in_enum': 'notes' in p['what']['enum'], 'topic': 'topic' in p, "
            "'instr': 'discover(what=' + chr(39) + 'notes' in (m.mcp.instructions or ''), "
            "'names': sorted(t['name'] for t in m.registered_tool_definitions())}))")
    return subprocess.run([sys.executable, "-c", code], cwd=str(HERE), capture_output=True, text=True,
                          timeout=180, env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), **env})


def test_off_removes_every_pointer_and_a_bad_switch_refuses_the_boot():
    r = _probe({})
    on = json.loads(r.stdout.strip().splitlines()[-1])
    assert on["notes_in_enum"] and on["topic"] and on["instr"] and len(on["names"]) == 13
    r = _probe({"CK3_NOTES": "off"})
    off = json.loads(r.stdout.strip().splitlines()[-1])
    assert off == dict(on, notes_in_enum=False, topic=False, instr=False)
    r = _probe({"CK3_NOTES": "maybe"})
    assert r.returncode == 3 and "notes_switch_invalid" in r.stderr


@pytest.fixture
def unconnected(monkeypatch):
    state = ms.BridgeState()
    state.connected = False
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def no_bridge(*_a, **_k):
        raise AssertionError("notes must not touch the bridge")
    monkeypatch.setattr(ms, "_query_bridge", no_bridge)
    monkeypatch.setattr(ms, "_action_bridge", no_bridge)
    monkeypatch.setattr(ms, "_try_reconnect", no_bridge)
    return state


def test_discover_notes_serves_index_and_topic_without_a_game(unconnected):
    env = run(ms.discover(what="notes"))
    assert env["ok"] is True, env
    assert env["result"]["version"] == N.version(SERVED)
    assert list(env["result"]["topics"]) == list(DOC["topics"])
    env = run(ms.discover(what="notes", topic="war"))
    assert env["ok"] is True, env
    want = [N.render(n) for n in SERVED["notes"] if n["topic"] == "war"]
    assert env["result"]["notes"] == want and env["result"]["topic"] == "war"
    assert len(json.dumps(env).encode()) <= N.TOPIC_REPLY_MAX + 400


def test_an_unknown_topic_is_refused_by_name(unconnected):
    env = run(ms.discover(what="notes", topic="weather"))
    assert env["ok"] is False and env["blocked"]["gate"] == "invalid_argument"
    assert env["blocked"]["fix"]["accepted"] == list(DOC["topics"])


def test_with_notes_off_discover_notes_is_an_unknown_subject(unconnected, monkeypatch):
    monkeypatch.setattr(ms, "_NOTES", N.boot({"CK3_NOTES": "off"}))
    unconnected.mock_mode = True          # past the connection check, as a live game would be
    env = run(ms.discover(what="notes"))
    assert env["ok"] is False and env["blocked"]["gate"] == "unknown_subject"


def test_session_health_reports_the_notes_version(monkeypatch):
    import redesign as R

    class GW:
        health = {"state": "ok"}

        async def read(self, action, params):
            return {}

        def game(self):
            return None
    env = run(R.session_impl(GW(), action="health", character_id=0, speed=1, enabled=None,
                             disable_native_ai=False, flag="", seconds=1, game_days=0))
    assert env["result"]["notes"] == {"state": "on", "version": N.version(SERVED),
                                      "notes": len(SERVED["notes"])}
