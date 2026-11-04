"""observe(), inspect(heir_plan) and inspect(guardian_candidates): behaviour against
a scripted gateway (cleanup 2026.11 F15).

All three resolve their actor the same way, so one table drives the actor gates
through each tool: actor_required (several rulers, no character_id),
no_ruler_bound and actor_unresolved (character_id=0 names nobody),
character_dead and cannot_act_as_player. bridge_disconnected is the tool
layer's own gate.

observe: the envelope and the brief/full sections, every read carrying the
character_id, the de-jure meters keyed on what the seat holds (two, one, none),
the standing facts, and an unknown depth refused (it used to read as brief).

heir_plan: the roster order (heir line, then children) and its cap, each row's
fields, the conqueror verdict and the groom list, a child read that names nobody
reported as a warning rather than an id-0 row, a childless ruler.

guardian_candidates: the four sources (council, spouses, court_of search, the
global pool), the in-court row winning a duplicate, the ruler left out, the
score, the order and the cap.

Both composed replies teach the player calls; a row reads every call they name
and checks the tool, intent, subject or domain exists.
"""

import asyncio
import json
import re
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

ms = pytest.importorskip("mcp_server")
import do_intent_args  # noqa: E402
import redesign as R  # noqa: E402
import tools as T  # noqa: E402

RULER = 30853


def run(coro):
    return asyncio.run(coro)


class Gw:
    """A scripted gateway: replies by bridge action (a dict, a callable of the
    params, or a per-character_id table under "by_cid"); every read recorded."""

    def __init__(self, replies=None, *, multi=False, chars=None):
        self.replies = dict(replies or {})
        # query_character's shape (build_char_profile): flat, `id`, gold, merit and
        # income_breakdown (r.key("merit"), r.key("income_breakdown"); L15 A3).
        self.chars = {0: {"id": RULER, "name": "Halfdan", "alive": True,
                          "gold": 250.0, "merit": 0.0,
                          "income_breakdown": {"total_monthly": 4.2}}}
        self.chars.update(chars or {})
        self.multi = multi
        self.reads = []
        self.health = "ok"

    async def actor_gate(self):
        return R.why_blocked("actor_required") if self.multi else None

    async def read(self, action, params=None):
        params = dict(params or {})
        self.reads.append((action, params))
        if action == "query_character":
            return self.chars.get(params.get("character_id", 0))
        r = self.replies.get(action)
        return r(params) if callable(r) else r


    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def write(self, action, params=None):
        raise AssertionError(f"a read-only tool wrote {action}")

    async def delta_records_since_last(self):
        return []

    def game(self):
        return {"tick": 99, "date": "867.4.1", "paused": True, "speed": 1}

    async def clock(self):                 # mcp_server._gateway_now reads it
        return self.game()


@pytest.fixture
def tool(monkeypatch):
    """Call a tool through mcp_server with `gw` as its gateway."""
    state = ms.BridgeState()
    state.connected = True
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    def call(gw, name, **kw):
        monkeypatch.setattr(ms, "_get_gateway", lambda: gw)
        return run(getattr(ms, name)(**kw))
    call.state = state
    return call


def gate_of(env):
    assert env["ok"] is False, env
    g = env["blocked"]["gate"]
    assert g in R.GATE_TABLE, g
    return g


# ── the actor gates, through each tool ────────────────────────────────────────

CALLS = {
    "observe": ("observe", {}),
    "heir_plan": ("inspect", {"subject": "heir_plan"}),
    "guardian_candidates": ("inspect", {"subject": "guardian_candidates"}),
}

ACTOR_CASES = {
    "actor_required": (dict(multi=True), {}),
    "no_ruler_bound": (dict(chars={0: {"success": False, "action": "query_character"}}), {}),
    "actor_unresolved": (dict(chars={0: None}), {}),
    "character_dead": (dict(chars={812: {"id": 812, "alive": False}}),
                       {"character_id": 812}),
}


@pytest.mark.parametrize("which", sorted(CALLS))
@pytest.mark.parametrize("gate", sorted(ACTOR_CASES))
def test_every_actor_gate_is_named_by_every_tool(tool, which, gate):
    name, kw = CALLS[which]
    gw_kw, extra = ACTOR_CASES[gate]
    gw = Gw(**gw_kw)
    env = tool(gw, name, **kw, **extra)
    assert gate_of(env) == gate
    assert env["tool"] == name
    assert [a for a, _ in gw.reads if a != "query_character"] == [], \
        "a refused actor reads nothing else"


@pytest.mark.parametrize("which", sorted(CALLS))
def test_control_a_bound_living_ai_ruler_passes_the_actor_gate(tool, which):
    name, kw = CALLS[which]
    env = tool(Gw(), name, **kw)
    assert env["ok"] is True, env.get("blocked")


@pytest.mark.parametrize("which", sorted(CALLS))
def test_no_bridge_is_bridge_disconnected(tool, monkeypatch, which):
    tool.state.connected = False

    async def no(_s):
        return False
    monkeypatch.setattr(ms, "_try_reconnect", no)
    name, kw = CALLS[which]
    gw = Gw()
    assert gate_of(tool(gw, name, **kw)) == "bridge_disconnected"
    assert gw.reads == []


# ── observe ───────────────────────────────────────────────────────────────────

BRIEF = {"de_jure", "standing", "wars", "money", "succession", "posture", "since"}
FULL = BRIEF | {"self", "war_brief", "council_brief", "evaluations"}


_TIER = {"b": 1, "c": 2, "d": 3, "k": 4, "e": 5}


def _realm(*keys, **more):
    """query_realm as do_query_realm writes it: the primary title's key and the
    held-title count; no titles list, no gold (L15)."""
    r = {"held_title_count": len(keys)}
    if keys:
        r["primary_title_key"] = keys[0]
    r.update(more)
    return r


def _seat(*keys, **more):
    """The two reads the de-jure meters key on: query_realm (the primary title)
    and query_title_hierarchy (held_titles, each with emit_title_info's key and
    tier)."""
    return {"query_realm": _realm(*keys, **more),
            "query_title_hierarchy": {"held_titles": [{"key": k, "tier": _TIER[k[0]]}
                                                      for k in keys]}}


def _meters(p):
    return {"title_key": p["title_key"], "held": 3, "total": 5, "fraction": 0.6}


def test_observe_brief_is_the_one_envelope(tool):
    gw = Gw({**_seat("d_york", "k_northumbria"),
             "query_dejure_progress": _meters,
             "get_ai_personality": {"boldness": 40}})
    env = tool(gw, "observe")
    assert env["ok"] is True and env["tool"] == "observe" and env["blocked"] is None
    assert set(env["result"]) == BRIEF
    assert env["actor"]["id"] == RULER and env["actor"]["name"] == "Halfdan"
    assert env["game"]["date"] == "867.4.1"
    assert isinstance(env["delta"], list)
    # L18 ruling (b): a bridge reply without lock_state reads not_reported
    assert env["result"]["posture"] == {"genome": {"boldness": 40, "lock_state": "not_reported"}}


@pytest.mark.parametrize("state", ["live", "quarantined", "none"])
def test_observe_posture_genome_carries_the_bridge_lock_state(tool, state):
    """L18 ruling (b): posture.genome.lock_state is the bridge's word
    (do_get_ai_personality's `lock_state`, L18-A), passed through."""
    gw = Gw({**_seat("d_york"), "query_dejure_progress": _meters,
             "get_ai_personality": {"boldness": 40, "lock_state": state, "world_epoch": 2}})
    genome = tool(gw, "observe")["result"]["posture"]["genome"]
    assert genome["lock_state"] == state and genome["world_epoch"] == 2, genome


def test_observe_full_adds_its_sections_and_the_war_stance_when_at_war(tool):
    gw = Gw({**_seat(), "get_active_wars": {"wars": [{"war_id": 3}]},
             "query_council": {"council": [{"position_key": "councillor_chancellor"}]},
             "get_ai_evaluations": {"evaluations": [{"kind": "war"}]}})
    r = tool(gw, "observe", depth="full")["result"]
    assert set(r) == FULL | {"war_stance_brief"}
    assert r["council_brief"] == [{"position_key": "councillor_chancellor"}]
    assert r["evaluations"] == {"evaluations": [{"kind": "war"}]}
    peace = tool(Gw({**_seat()}), "observe", depth="full")["result"]
    assert set(peace) == FULL


def test_observe_full_self_carries_the_faith_and_culture_keys(tool):
    """L14 FF2 N-14: query_character sends the display names and the scripted
    keys (emit_char_names, dispatch_router.cpp: culture/culture_key,
    faith/faith_key); self carries both, as doctrine teaches the keys. The
    values are Ivar's, read off the 867 dump (the round-2 witness). RED on
    b84e3634 (no key fields)."""
    gw = Gw({**_seat()},
            chars={0: {"id": RULER, "name": "Ivar", "alive": True,
                       "faith": "\u00c1satr\u00fa", "faith_key": "norse_pagan",
                       "culture": "Norse", "culture_key": "norse"}})
    me = tool(gw, "observe", depth="full")["result"]["self"]
    assert (me["faith_key"], me["culture_key"]) == ("norse_pagan", "norse"), me
    assert me["culture"] == "Norse", me
    # an absent key is null, never the display name standing in for it
    me = tool(Gw({**_seat()}), "observe", depth="full")["result"]["self"]
    assert me["faith_key"] is None and me["culture_key"] is None, me


def test_observe_full_self_titles_held_is_query_realms_held_title_count(tool):
    """L15 (witness r4 N4-1): self.titles_held read realm.titles_held /
    title_count, which do_query_realm never writes, so every seat read 0 (l14i
    geval-p eval_e_names: Ludwig, 61 counties, titles_held 0). It is
    held_title_count. RED on 627af763 (0)."""
    gw = Gw({**_seat("k_france", "d_paris", "c_paris")})
    me = tool(gw, "observe", depth="full")["result"]["self"]
    assert me["titles_held"] == 3, me
    assert "government" not in me, "no reply carries the government: no null stands in for it"


@pytest.mark.parametrize("depth", ["deep", "Full", "", "everything"])
def test_observe_an_unknown_depth_is_refused_and_nothing_is_read(tool, depth):
    gw = Gw()
    env = tool(gw, "observe", depth=depth)
    assert gate_of(env) == "invalid_argument"
    assert "brief" in env["blocked"]["summary"] and "full" in env["blocked"]["summary"]
    assert gw.reads == []


def test_observe_every_read_carries_the_character_id(tool):
    gw = Gw({**_seat("d_york"), "query_dejure_progress": _meters,
             "get_active_wars": {"wars": [{"war_id": 3}]}},
            chars={812: {"id": 812, "alive": True}})
    tool(gw, "observe", character_id=812, depth="full")
    with_cid = [(a, p) for a, p in gw.reads if "character_id" in p]
    assert len(with_cid) >= 9, gw.reads
    assert {p["character_id"] for _, p in with_cid} == {812}, with_cid


def test_observe_de_jure_meters_are_the_titles_the_seat_holds(tool):
    gw = Gw({**_seat("d_york", "k_northumbria"),
             "query_dejure_progress": _meters})
    dj = tool(gw, "observe")["result"]["de_jure"]
    assert dj["primary_title"]["title"] == "d_york" and dj["second_title"]["title"] == "k_northumbria"
    assert dj["primary_title"]["need"] == 3 and dj["primary_title"]["met"] is True
    asked = [p["title_key"] for a, p in gw.reads if a == "query_dejure_progress"]
    assert asked == ["d_york", "k_northumbria"]


def test_observe_one_held_title_and_none(tool):
    one = tool(Gw({**_seat("d_york"), "query_dejure_progress": _meters}),
               "observe")["result"]["de_jure"]
    assert one["primary_title"]["title"] == "d_york"
    assert one["second_title"]["unavailable"]
    gw = Gw({**_seat()})
    none = tool(gw, "observe")["result"]["de_jure"]
    assert none["primary_title"]["unavailable"] and none["second_title"]["unavailable"]
    assert none["can_mint_now"] is False
    assert [a for a, _ in gw.reads if a == "query_dejure_progress"] == [], \
        "no guessed title is read"


@pytest.mark.parametrize("failed", [
    None,                                                         # no reply
    {"error": "no_title_realm_data", "character_id": RULER},      # the handler's refusal
    {"success": False, "action": "query_title_hierarchy"},        # dispatch failure
], ids=["absent", "error", "declined"])
def test_observe_a_failed_held_titles_read_says_unread_not_one_title(tool, failed):
    """L15 witness m3: when query_title_hierarchy FAILED, observe said "this seat
    holds only one resolvable title": nobody looked, told as a count. RED on
    03f19352 (the count line)."""
    seat = {**_seat("d_york"), "query_title_hierarchy": failed,
            "query_dejure_progress": _meters}
    second = tool(Gw(seat), "observe")["result"]["de_jure"]["second_title"]
    assert second["unavailable"].startswith("unread"), second
    assert "only one" not in second["unavailable"], second
    # the control: a read that names one held title still says so
    one = tool(Gw({**_seat("d_york"), "query_dejure_progress": _meters}),
               "observe")["result"]["de_jure"]["second_title"]
    assert one["unavailable"] == "this seat holds only one resolvable title", one


def test_observe_a_clamped_held_titles_list_says_unread_not_one_title(tool):
    """L15 witness n6: do_query_title_hierarchy zeroed a title count above 500 and
    still emitted `held_titles: []`, while do_query_realm writes the count
    unclamped (`held_title_count`). RED on 16dd7f14: the empty list read as a
    successful read, "holds only one". Since L16 R9 the emitter answers an error
    instead; this is the reader's second line of defence."""
    seat = {"query_realm": _realm("d_york", held_title_count=612),
            "query_title_hierarchy": {"character_id": RULER, "held_titles": [],
                                      "held_title_count": 0},
            "query_dejure_progress": _meters}
    second = tool(Gw(seat), "observe")["result"]["de_jure"]["second_title"]
    assert second["unavailable"].startswith("unread"), second
    assert "clamped" in second["unavailable"], second
    # the control: an empty list with query_realm counting none is a read
    seat["query_realm"] = _realm(held_title_count=0)
    none = tool(Gw(seat), "observe")["result"]["de_jure"]["second_title"]
    assert not none["unavailable"].startswith("unread"), none


#: L16 (ruling R9): what do_query_title_hierarchy answers now instead of an
#: empty list. Keys from the emitter: `character_id` (query_handlers.cpp
#: do_query_title_hierarchy, written before the count is read), then
#: title_count_clamped's `error`/`count_field`/`raw_count`/`max_count`, or the
#: lookup failure's `error`/`title_id`/`index`/`raw_count`.
CLAMPED = {"character_id": RULER, "error": "title_count_clamped",
           "count_field": "held_title_count", "raw_count": 612, "max_count": 500}
LOOKUP_FAILED = {"character_id": RULER, "error": "title_lookup_failed",
                 "title_id": 4242, "index": 1, "raw_count": 3}


@pytest.mark.parametrize("reply,err", [(CLAMPED, "title_count_clamped"),
                                       (LOOKUP_FAILED, "title_lookup_failed")],
                         ids=["clamped", "lookup_failed"])
def test_observe_names_the_bridges_held_titles_error(tool, reply, err):
    """L16 R9: the emitter answers a clamp or a skipped lookup as an error with
    the raw count, and observe says which, by the bridge's own word. RED on
    24cac589 (the generic "failed or came back empty ... (clamped)" line)."""
    seat = {"query_realm": _realm("d_york", held_title_count=reply["raw_count"]),
            "query_title_hierarchy": dict(reply), "query_dejure_progress": _meters}
    second = tool(Gw(seat), "observe")["result"]["de_jure"]["second_title"]
    assert second["unavailable"].startswith("unread"), second
    assert f"answered {err} (raw count {reply['raw_count']})" in second["unavailable"], second
    assert "only one" not in second["unavailable"], second


def test_hierarchy_was_read_refuses_the_emitted_errors_whatever_the_realm_says():
    """L16 R9: the reader's L15 defence and the emitter's error agree. An error
    reply is unread even when query_realm counts none (so the realm check is
    not what refuses it); the control, a real list, reads."""
    for reply in (CLAMPED, LOOKUP_FAILED):
        for realm in ({"held_title_count": reply["raw_count"]}, {"held_title_count": 0}, None):
            assert R.hierarchy_was_read(dict(reply), realm) is False, (reply, realm)
    ok = {"character_id": RULER, "held_titles": [{"title_id": 1, "key": "d_york"}],
          "held_title_count": 1}
    assert R.hierarchy_was_read(ok, {"held_title_count": 1}) is True


def test_observe_standing_reports_debt_war_and_the_faction_line(tool):
    gw = Gw({**_seat(),
             "get_active_wars": {"wars": [{"war_id": 3, "war_score": 0,
                                           "is_attacker": False}]},
             "query_faction_threat": {"factions": [{"power_pct_of_liege": 85}]}},
            chars={0: {"id": RULER, "name": "Halfdan", "alive": True, "gold": -75.0}})
    r = tool(gw, "observe")["result"]
    st = r["standing"]
    assert st["debt"] == 75 and st["at_war"] is True and st["faction_max_pct"] == 85
    assert r["money"]["in_debt"] is True
    (war,) = r["wars"]["active"]
    assert war["war_score"] == 0 and war["my_side"] == "defender"
    assert "score_is_uninformative" not in war and "sieges_in_progress" not in war


def test_observe_depth_is_described_with_its_values():
    desc = {t["name"]: t for t in ms.registered_tool_definitions()}["observe"]
    assert desc["input_schema"]["properties"]["depth"]["enum"] == list(R.OBSERVE_DEPTHS)
    assert "Any other value is refused" in desc["description"]


# ── heir_plan ─────────────────────────────────────────────────────────────────

def _family(extra=None):
    """The ruler, two heirs in the simulation line, three children (one also an
    heir), and their education reads."""
    # query_character's shape (build_char_profile): `id`, skills nested, traits as
    # {"name": ...} objects, children bare ids.
    chars = {
        0: {"id": RULER, "name": "Halfdan", "alive": True,
            "children": [101, 102, 103], "primary_title_id": 77},
        RULER: {"id": RULER, "name": "Halfdan", "alive": True,
                "children": [101, 102, 103], "primary_title_id": 77},
        201: {"id": 201, "name": "Ivar", "age": 30, "skills": {"martial": 19},
              "traits": [{"name": "brave"}]},
        101: {"id": 101, "name": "Ubba", "age": 9, "skills": {"martial": 6},
              "traits": [{"name": "rowdy"}]},
        102: {"id": 102, "name": "Sigurd", "age": 12, "skills": {"martial": 4},
              "traits": []},
        103: {"id": 103, "name": "Bjorn", "age": 20, "skills": {"martial": 9},
              "traits": []},
    }
    chars.update(extra or {})
    replies = {
        # The emitter's keys (query_handlers.cpp do_query_succession_simulation:
        # action, character_id, primary_title_id, primary_title_tier, heirs, player
        # heir first). It writes no succession law, so heir_plan reports none.
        "query_succession_simulation": {"action": "query_succession_simulation",
                                        "character_id": RULER, "primary_title_id": 77,
                                        "primary_title_tier": 3,
                                        "heirs": [{"character_id": 201, "is_player_heir": True},
                                                  {"character_id": 101}]},
        "query_heir_education": lambda p: {"education_focus": f"focus_{p['character_id']}",
                                           "education_focus_skill": "martial",
                                           "guardian_id": None},
    }
    return replies, chars


def _heirs(tool, max_results=12, extra=None):
    replies, chars = _family(extra)
    gw = Gw(replies, chars=chars)
    env = tool(gw, "inspect", subject="heir_plan", max_results=max_results)
    return env, gw


def test_heir_plan_orders_the_heir_line_then_the_children(tool):
    env, gw = _heirs(tool)
    assert env["ok"] is True and env["tool"] == "inspect"
    r = env["result"]
    assert r["subject"] == "heir_plan"
    assert [h["id"] for h in r["heirs"]] == [201, 101, 102, 103]
    assert r["heir_count"] == 2 and r["child_count"] == 3
    assert r["primary_title_id"] == 77
    # L15 m1: no emitter writes a law into the simulation; the field is gone,
    # not a null that reads as "no law".
    assert "succession_law" not in r
    assert ("query_succession_simulation", {"character_id": 0}) in gw.reads


def test_heir_plan_row_fields(tool):
    rows = {h["id"]: h for h in _heirs(tool)[0]["result"]["heirs"]}
    ivar, ubba, sigurd, bjorn = rows[201], rows[101], rows[102], rows[103]
    assert ivar["is_primary_heir"] and ivar["in_heir_line"] and not ivar["is_child"]
    assert ubba["in_heir_line"] and ubba["is_child"] and not ubba["is_primary_heir"]
    assert not sigurd["in_heir_line"] and sigurd["is_child"]
    assert not bjorn["is_child"], "an adult child is not a child to groom"
    assert ubba["martial_affinity_child_traits"] == ["rowdy"]
    assert ubba["education_focus"] == "focus_101" and ubba["education_unresolved"] is False
    assert ubba["guardian_id"] is None and ubba["guardian_unresolved"] is True
    assert ubba["projected_adult_martial_estimate"] is None


def test_heir_plan_conqueror_verdict_and_groom_list(tool):
    env, _ = _heirs(tool)
    r = env["result"]
    rows = {h["id"]: h for h in r["heirs"]}
    assert rows[201]["conqueror_gate"]["verdict"] == "conqueror"
    assert rows[201]["conqueror_gate_martial_ge_18"] is True
    assert r["has_conqueror_heir"] is True
    assert [g["id"] for g in r["groom_targets"]] == [101, 102]
    assert not any(w["code"] == "no_conqueror_heir" for w in env["warnings"])


def test_heir_plan_without_a_conqueror_warns(tool):
    env, _ = _heirs(tool, extra={201: {"id": 201, "age": 30,
                                       "skills": {"martial": 8}, "traits": []}})
    assert env["result"]["has_conqueror_heir"] is False
    assert any(w["code"] == "no_conqueror_heir" for w in env["warnings"])


def test_heir_plan_max_results_caps_the_roster(tool):
    env, gw = _heirs(tool, max_results=2)
    assert [h["id"] for h in env["result"]["heirs"]] == [201, 101]
    assert [p["character_id"] for a, p in gw.reads if a == "query_heir_education"] == [201, 101]


@pytest.mark.parametrize("bad", [None, {}, {"success": False, "action": "query_character"},
                                 {"id": 0}])
def test_heir_plan_a_child_read_that_names_nobody_is_a_warning_not_a_row(tool, bad):
    env, _ = _heirs(tool, extra={102: bad})
    ids = [h["id"] for h in env["result"]["heirs"]]
    assert ids == [201, 101, 103], ids
    assert 0 not in ids
    (w,) = [w for w in env["warnings"] if w["code"] == "heir_read_failed"]
    assert "102" in w["detail"]


def test_heir_plan_a_childless_ruler_says_so(tool):
    gw = Gw({"query_succession_simulation": {"heirs": []}})
    env = tool(gw, "inspect", subject="heir_plan")
    assert env["ok"] is True and env["result"]["heirs"] == []
    assert [w["code"] for w in env["warnings"]] == ["no_heirs_or_children"]


# ── guardian_candidates ───────────────────────────────────────────────────────

def _court():
    # query_character (build_char_profile) and search_characters rows: `id`, skills
    # nested under `skills`, spouses as {id, alive} objects; a profile's traits are
    # {"name": ...} objects, a search row's are names.
    chars = {
        0: {"id": RULER, "alive": True, "spouses": [{"id": 301, "alive": True}]},
        RULER: {"id": RULER, "alive": True, "spouses": [{"id": 301, "alive": True}]},
        401: {"id": 401, "name": "Marshal", "skills": {"martial": 20, "learning": 5},
              "age": 40, "traits": [{"name": "shrewd"}]},
        301: {"id": 301, "name": "Wife", "skills": {"martial": 8, "learning": 15},
              "age": 30, "traits": [{"name": "dull"}]},
    }

    def search(p):
        if p.get("court_of"):
            return {"characters": [
                {"id": 501, "name": "Courtier", "skills": {"martial": 12, "learning": 0},
                 "age": 25},
                {"id": RULER, "name": "Halfdan", "skills": {"martial": 30}}]}
        return {"characters": [
            {"id": 601, "name": "Far King", "skills": {"martial": 25, "learning": 0}, "age": 50},
            {"id": 401, "name": "Marshal", "skills": {"martial": 20, "learning": 5}, "age": 40}]}
    # query_council as do_query_council writes it (query_handlers.cpp :5701/:5703,
    # rows :5786-:5803: position_id, position_key, holder_id, vacant). L15 A2: the
    # double was {"seats": [{"character_id": 401}]}, a shape the bridge never
    # writes, and it hid a council arm that found nobody live.
    replies = {"query_council": {"character_id": RULER, "council": [
                   {"position_id": 1, "position_key": "councillor_marshal",
                    "position_name": "councillor_marshal", "current_task": "",
                    "holder_id": 401, "holder_source": "seated"},
                   {"position_id": 2, "position_key": "councillor_spymaster",
                    "position_name": "councillor_spymaster", "current_task": "",
                    "holder_id": 16777215, "vacant": True}]},
               "search_characters": search}
    return replies, chars


def _guardians(tool, max_results=10):
    replies, chars = _court()
    gw = Gw(replies, chars=chars)
    return tool(gw, "inspect", subject="guardian_candidates", max_results=max_results), gw


def test_guardian_candidates_reads_the_four_sources(tool):
    env, gw = _guardians(tool)
    assert env["ok"] is True and env["tool"] == "inspect"
    r = env["result"]
    assert r["subject"] == "guardian_candidates" and r["ruler_id"] == RULER
    court = {c["id"]: c["source"] for c in r["in_court_candidates"]}
    assert court == {401: "council", 301: "spouse", 501: "court"}
    assert [c["id"] for c in r["aspirational_global"]] == [601]
    searches = [p for a, p in gw.reads if a == "search_characters"]
    assert searches[0] == {"filter": "alive", "sort": "martial", "sort_order": "desc",
                           "max_results": 10, "court_of": RULER}
    assert "court_of" not in searches[1]


def test_guardian_candidates_in_court_wins_a_duplicate_and_the_ruler_is_left_out(tool):
    r = _guardians(tool)[0]["result"]
    every = r["in_court_candidates"] + r["aspirational_global"]
    assert [c["id"] for c in every].count(401) == 1
    assert next(c for c in every if c["id"] == 401)["in_court"] is True
    assert RULER not in [c["id"] for c in every]


def test_guardian_candidates_score_and_order(tool):
    r = _guardians(tool)[0]["result"]
    by = {c["id"]: c for c in r["in_court_candidates"]}
    assert by[401]["guardian_score"] == round(0.4 * 20 + 0.2 * 5 + 5, 2)
    assert by[401]["good_intellect"] is True
    assert by[301]["guardian_score"] == round(0.4 * 8 + 0.2 * 15 - 5, 2)
    assert by[301]["bad_intellect"] is True
    scores = [c["guardian_score"] for c in r["in_court_candidates"]]
    assert scores == sorted(scores, reverse=True)


def test_guardian_candidates_max_results_caps_each_list(tool):
    r = _guardians(tool, max_results=1)[0]["result"]
    assert [c["id"] for c in r["in_court_candidates"]] == [401]
    assert len(r["aspirational_global"]) == 1


# ── what the replies teach exists ─────────────────────────────────────────────

CALL = re.compile(r"\b(do|inspect|query|discover|succession_plan|act|observe|steer)\(([^)]*)\)")


def _taught(text):
    out = []
    for tool_name, args in CALL.findall(text):
        # either quote style: a call cannot evade the check by its quotes
        kv = {k: v for k, _, v in re.findall(r"(\w+)=(['\"])(.*?)\2", args)}
        out.append((tool_name, kv))
    return out


def _check_taught(env):
    text = json.dumps(env, ensure_ascii=False).replace('\\"', '"')
    calls = _taught(text)
    assert calls, "the reply teaches no call: the census is stale (fail OPEN)"
    tools = {t["name"] for t in ms.registered_tool_definitions()}
    interact_args = set(re.findall(r"\w+", do_intent_args.table_text().split("interact:", 1)[1]
                                   .split("\n", 1)[0]))
    for name, kv in calls:
        assert name in tools, f"the reply teaches {name}(), which is not a tool"
        if name == "do":
            assert kv.get("intent") in R.INTENTS, kv
            if kv.get("intent") == "interact":
                for key in ("interaction", "target_id", "secondary_actor_id",
                            "secondary_recipient_id"):
                    assert key in interact_args, key
        if name == "inspect":
            assert kv.get("subject") in R.INSPECT_SUBJECTS, kv
        if name == "query":
            assert kv.get("domain") in T.QUERY_DOMAINS, kv
    assert "good_guardian_candidates" not in text
    assert "act type=" not in text
    return calls


def test_heir_plan_teaches_only_calls_that_exist(tool):
    env, _ = _heirs(tool)
    calls = _check_taught(env)
    assert ("inspect", {"subject": "guardian_candidates"}) in calls
    assert any(n == "do" and kv.get("intent") == "lifestyle" for n, kv in calls)


def test_guardian_candidates_teaches_only_calls_that_exist(tool):
    env, _ = _guardians(tool)
    _check_taught(env)
    text = json.dumps(env["result"])
    assert "once the assign lever" not in text and "EXPERIMENTAL" not in text


def test_inspect_takes_no_include():
    """inspect's `include` was accepted and never read; it is deleted, from the
    schema the model sees and from the implementation."""
    import inspect as _inspect
    props = {t["name"]: t for t in ms.registered_tool_definitions()}["inspect"]["input_schema"]["properties"]
    assert "include" not in props
    assert "include" not in _inspect.signature(R.inspect_impl).parameters
