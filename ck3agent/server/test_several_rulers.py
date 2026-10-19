"""lane-multi: several rulers in ONE session.

Two halves. The pre-bind: `CK3_AGENT_CHARACTER_ID` takes a comma-separated list,
the first is claimed as the primary and every further one is an ADDED claim
(`add: true`), so the bridge does not switch away from the first. And
`session(action="add")`: one more ruler, the primary kept, an older bridge that
ignores `add` (and therefore SWITCHES) reported as such rather than as a bind.
"""
import asyncio
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import mcp_server  # noqa: E402
import redesign  # noqa: E402


class _Client:
    async def handshake(self, **kw):
        return {"status": "ok"}


def _run(coro):
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(coro)


def _bind(monkeypatch, env, reply=None):
    sent = []

    async def _fake_action(state, action, params):
        sent.append((action, dict(params)))
        if reply is not None:
            return reply(params)
        return {"success": True, "added": bool(params.get("add"))}

    monkeypatch.setattr(mcp_server, "_action_bridge", _fake_action)
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, env)
    st = mcp_server.BridgeState()
    _run(mcp_server._handshake_and_bind(_Client(), st))
    return st, sent


def test_a_list_binds_the_first_as_primary_and_ADDS_the_rest(monkeypatch):
    st, sent = _bind(monkeypatch, "13902, 12145")
    assert sent == [
        ("set_controlled_character", {"character_id": 13902, "disable_native_ai": False}),
        ("set_controlled_character", {"character_id": 12145, "disable_native_ai": False,
                                      "add": True}),
    ]
    assert st.prebind["character_id"] == 13902 and st.prebind["ok"] is True
    assert st.prebind["also"] == [{"character_id": 12145, "ok": True, "reason": None,
                                   "bound_at": {"tick": None, "date": None}}]


def test_a_single_id_is_unchanged_and_carries_no_also(monkeypatch):
    st, sent = _bind(monkeypatch, "30853")
    assert len(sent) == 1 and "add" not in sent[0][1]
    # lane-prebind added `bound_at` (the reply's clock stamp; the fake has none)
    # and `wait` (nothing waited: the fake ack names no session_state).
    assert st.prebind == {"character_id": 30853, "ok": True, "reason": None,
                          "bound_at": {"tick": None, "date": None},
                          "wait": {"waited_s": 0.0, "polls": 0, "game_active": None}}


@pytest.mark.parametrize("bad", ["13902,harold", "13902,,12145", "13902,-4", "1,2,3,4,5,6,7,8,9,10,11"])
def test_one_bad_entry_claims_NOTHING(monkeypatch, bad):
    """Refuse rather than bind part of a set the declaration meant whole."""
    st, sent = _bind(monkeypatch, bad)
    assert sent == [] and getattr(st, "prebind", None) is None


def test_a_repeated_id_is_bound_once(monkeypatch):
    st, sent = _bind(monkeypatch, "13902,13902,12145")
    assert [p["character_id"] for _, p in sent] == [13902, 12145]


def test_an_OLD_bridge_that_ignores_add_is_recorded_as_a_refusal(monkeypatch):
    """It answers success but switched; `added` is absent, so it is not a bind."""
    st, sent = _bind(monkeypatch, "13902,12145", reply=lambda p: {"success": True})
    assert st.prebind["ok"] is True
    assert st.prebind["also"] == [{"character_id": 12145, "ok": False,
                                   "reason": "add_unsupported_by_bridge"}]


def test_an_added_ruler_refused_does_not_unbind_the_first(monkeypatch):
    st, _ = _bind(monkeypatch, "13902,12145", reply=lambda p: (
        {"success": False, "reason": "owned_by_other"} if p.get("add")
        else {"success": True}))
    assert st.prebind["ok"] is True
    assert st.prebind["also"][0] == {"character_id": 12145, "ok": False,
                                     "reason": "owned_by_other"}


def test_the_prebind_is_written_into_the_session_record(monkeypatch, tmp_path):
    rec = tmp_path / "s.session.json"
    rec.write_text(json.dumps({"session_id": "s1", "resume_token": "t"}))
    monkeypatch.setenv(mcp_server.SESSION_OUT_ENV, str(rec))
    monkeypatch.setenv(mcp_server.RELEASE_ON_EXIT_ENV, str(tmp_path / "rel.json"))
    _bind(monkeypatch, "13902,12145")
    got = json.loads(rec.read_text())
    assert got["session_id"] == "s1" and got["resume_token"] == "t"
    none = {"tick": None, "date": None}          # the fake's replies carry no clock stamp
    assert got["prebind"] == [{"character_id": 13902, "ok": True, "reason": None, "bound_at": none},
                              {"character_id": 12145, "ok": True, "reason": None, "bound_at": none}]
    assert got["prebind_wait"] == {"waited_s": 0.0, "polls": 0, "game_active": None}
    assert (rec.stat().st_mode & 0o777) == 0o600


# ── session(action="add") ────────────────────────────────────────────────────

class _Gw:
    def __init__(self, reply):
        self.reply, self.health, self.writes = reply, "ok", []

    async def write(self, action, params):
        self.writes.append((action, dict(params)))
        return self.reply

    async def read(self, action, params):
        if action == "query_character":
            return {"character_id": params.get("character_id"), "name": "Boris", "alive": True}
        return {}

    def game(self):
        return {"tick": 1, "date": "867.1.1", "paused": True, "speed": 1}


def _add(reply, cid=12145):
    gw = _Gw(reply)
    return gw, _run(redesign.session_impl(gw, "add", character_id=cid))


def test_add_sends_add_true_and_reports_every_claim():
    gw, out = _add({"success": True, "added": True, "controlled_primary": 13902,
                    "claimed_character_ids": [12145, 13902]})
    assert gw.writes == [("set_controlled_character",
                          {"character_id": 12145, "disable_native_ai": False, "add": True})]
    assert out["ok"] is True and out["verdict"] == "applied", out
    r = out["result"]
    assert r["primary_character_id"] == 13902 and r["my_claims"] == [12145, 13902]
    assert r["add_witnessed"] is True and r["added"]["name"] == "Boris"


def test_add_against_an_old_bridge_says_it_may_have_switched():
    _, out = _add({"success": True, "controlled_primary": 12145})
    assert out["ok"] is False and out["blocked"]["gate"] == "add_unsupported_by_bridge"
    assert out["blocked"]["retryable"] is False


def test_add_refusal_is_typed():
    _, out = _add({"success": False, "reason": "watchlist_full"})
    assert out["ok"] is False and out["blocked"]["gate"] == "watchlist_full"


def test_add_without_an_id_is_refused_before_the_bridge():
    gw, out = _add({"success": True, "added": True}, cid=0)
    assert out["ok"] is False and out["blocked"]["gate"] == "missing_character_id"
    assert gw.writes == []
