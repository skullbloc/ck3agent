"""A refused write carries the game's own sentence (L14 finding 1).

The evaluator granted a county to Ludwig's heir and read `blocked: {gate:
"interaction_invalid", summary: "send_conditions_not_met"}`; the game's words
("You cannot grant your Realm Capital; ...") were one call away, in
inspect(pending). The bridge's query_dispatch_result carries no detail
(action_handlers.cpp do_query_dispatch_result); its completed ring does
(query_intent_results `detail`). The inverse case: set_title_law's reply said
"engine_refused: Missing prestige 960" while its pending row said "the game gives
no detail".

One function builds `blocked` for a refused write: redesign.write_blocked. This
file enumerates the write paths from the code (every function in redesign.py that
dispatches a write) and drives each one that can carry the game's words with a
fake whose dispatch result has NO detail and whose ring row does.
"""
import ast
import asyncio
import os

import pytest

import factions
import redesign as R

ME, FOE, WAR = 100, 200, 16777282
RID = "si_100_1_1"
MARKUP = ("\x16warning_icon! \x15X You cannot grant your Realm Capital\x15!\n"
          "\x16warning_icon! \x15X Women must be a Dominant, Equal, or Clerical Gender\x15!")
WORDS = R.engine_text(MARKUP)


@pytest.fixture(autouse=True)
def _instant(monkeypatch):
    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    getattr(R, "_REPLY_REFUSAL_WORDS", {}).clear()


class Bridge:
    """The real bridge's two shapes: query_dispatch_result answers the verdict with
    no detail; query_intent_results answers the ring row with the detail."""

    def __init__(self, send_reply=None, ring_detail=MARKUP, reason="send_conditions_not_met",
                 ring=True, replies=None):
        self.send_reply = send_reply or {"success": True, "status": "queued",
                                         "intent_request_id": RID}
        self.ring_detail, self.reason, self.ring = ring_detail, reason, ring
        # query_create_title_cost: the price the mint gate reads before it sends
        # (L14 S3). Priced and affordable here, so the mint goes out and the GAME
        # refuses it, which is the path these rows drive.
        self.replies = {"query_wars": {"scope": "all_active", "wars": [_WAR]},
                        # declare_war's pre-read, in the emitter's keys
                        # (do_get_active_wars): no war yet. L15 m7: a read
                        # without the list now refuses before the send.
                        "get_active_wars": {"success": True, "action": "get_active_wars",
                                            "character_id": ME, "wars": [], "war_count": 0},
                        "query_create_title_cost": {
                            "success": True, "action": "query_create_title_cost",
                            "actor_id": ME, "title_key": "k_york", "engine_valid": True,
                            "engine_reasons": [],
                            "price": {"known": True, "currency": "gold", "cost": 10.0,
                                      "have": 50.0, "affordable": True}},
                        **(replies or {})}
        self.health = "ok"
        self.writes = []

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        if action in self.replies:
            r = self.replies[action]
            return r(params) if callable(r) else r
        if action == "query_character":
            return {"id": (params or {}).get("character_id") or ME, "alive": True,
                                  "name": "seat", "gold": 50, "prestige": 300, "age": 30}
        if action == "query_dispatch_result":
            return {"success": True, "status": "rejected", "rejection_reason": self.reason,
                    "dispatch_id": (params or {}).get("dispatch_id"), "actor_id": ME}
        if action == "query_intent_results":
            if not self.ring:
                return {}
            row = {"intent_id": RID, "status": "rejected", "rejection_reason": self.reason}
            if self.ring_detail is not None:
                row["detail"] = self.ring_detail
            return {"results": [row]}
        if action == "get_tick_info":
            return {"paused": True, "game_date": "867.01.05", "tick_id": 1}
        return {}

    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return self.send_reply

    async def clock(self):
        return R.clock_of(await self.read("get_tick_info", {}))

    async def confirmed_write(self, action, params, *, readback, deadline_s=12.0, max_polls=8,
                              poll_interval_s=None):
        return await R.confirm_write(self.clock, self.read,
                                     lambda: self.write(action, params), readback,
                                     deadline_s=deadline_s, max_polls=max_polls)

    def game(self):
        return {"tick": 1, "date": "867.01.05", "paused": True}


def run(coro):
    return asyncio.run(coro)


_WAR = {"war_id": WAR, "name": "War", "primary_attacker": ME, "primary_defender": FOE,
        "attackers": [ME], "defenders": [FOE]}

#: Each write path that sends a write the GAME can refuse, driven to that refusal.
DRIVERS = {
    "do_impl": lambda gw: R.do_impl(gw, intent="interact", character_id=ME,
                                    args={"interaction": "grant_titles_interaction",
                                          "target_id": FOE, "title_id": 1141}),
    "_do_scheme": lambda gw: R.do_impl(gw, intent="scheme", character_id=ME,
                                       args={"action": "start", "scheme": "befriend",
                                             "target_id": FOE}),
    "_do_war_intent": lambda gw: R.do_impl(gw, intent="make_peace", character_id=ME,
                                           args={"war_id": WAR, "terms": "enforce_demands"}),
    "declare_war_impl": lambda gw: R.declare_war_impl(gw, ME, target_id=FOE, cb="claim_cb",
                                                      confirm_straddle=True),
    "_confirm_intent_write": lambda gw: R.do_impl(gw, intent="mint_title", character_id=ME,
                                                  args={"title_key": "k_york"}),
}

#: Write paths whose refusal is never the game's sentence, and why. They still
#: build `blocked` through write_blocked (the structural test below).
NOT_THE_GAMES = {
    "_do_cancel_pending": "the bridge's own queue answers a cancel (not_found), not the game",
    "_set_experimental_flag": "a bridge switch; the game is not asked",
    "set_phase_impl": "writes personality weights and a bridge flag; the bridge refuses with its own codes",
    "_session_impl_branches": "bridge session verbs; their refusals are the bridge's own codes",
    "_session_clock_write": "session pause/unpause; the bridge's own clock switch refuses with its codes",
    "_session_speed_write": "session speed; the bridge's own clock switch refuses with its codes",
    "steer_impl": "set_ai_personality is the bridge's own synchronous write; it refuses with its codes",
}

#: The machinery every path above goes through (not a path of its own).
MACHINERY = {"confirm_write", "settle_write", "_confirm_write", "write"}

_WRITE_CALLS = {"settle_write", "await_drain", "_confirm_write", "confirmed_write", "write",
                "confirm_write"}


def _functions():
    tree = ast.parse(open(R.__file__, encoding="utf-8").read())
    for fn in ast.walk(tree):
        if isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            names = set()
            for n in ast.walk(fn):
                if isinstance(n, ast.Call):
                    f = n.func
                    names.add(f.attr if isinstance(f, ast.Attribute) else getattr(f, "id", None))
            yield fn.name, names


def _write_paths():
    return {name: calls for name, calls in _functions() if calls & _WRITE_CALLS}


def test_every_write_path_is_enumerated_and_accounted_for():
    """A new write path reds here until it is driven (DRIVERS) or said not to carry
    the game's words (NOT_THE_GAMES)."""
    found = set(_write_paths()) - MACHINERY
    assert found == set(DRIVERS) | set(NOT_THE_GAMES), (
        f"unaccounted: {sorted(found - set(DRIVERS) - set(NOT_THE_GAMES))}; "
        f"gone: {sorted(set(DRIVERS) | set(NOT_THE_GAMES) - found)}")


def test_no_write_path_builds_its_refusal_around_write_blocked():
    """Only write_blocked maps a write's refusal; settle_write only classifies one."""
    for name, calls in _write_paths().items():
        if name == "settle_write":
            continue
        assert "map_raw_reason" not in calls, f"{name} maps a refusal itself"


# ── L14 FF1 m-8: every server module, and why_blocked as well as map_raw_reason ──
#: The builders themselves (they are where a refusal is built).
_BUILDERS = {"write_blocked", "_with_words", "lever_absent_block", "settle_write",
             "confirm_write", "_confirm_write", "write"}
_HAND_BUILT = ("why_blocked", "map_raw_reason")


def _call_name(c):
    f = c.func
    return f.attr if isinstance(f, ast.Attribute) else getattr(f, "id", None)


def _has_write(node) -> bool:
    return any(isinstance(x, ast.Call) and _call_name(x) in _WRITE_CALLS for x in ast.walk(node))


def _exclusive_branch(stmt) -> bool:
    """`if ...: ...; return` with no else: a branch the statements after it do not
    follow (a session verb's own branch), so its write does not reach them."""
    return (isinstance(stmt, ast.If) and bool(stmt.body) and not stmt.orelse
            and isinstance(stmt.body[-1], ast.Return))


def hand_built_after_a_write(fn) -> list:
    """(line, call) of each why_blocked / map_raw_reason a statement reached after
    a write in `fn` makes: a refusal of what the write did, which write_blocked
    must build. A refusal before the write (an argument check) is not one."""
    out = []

    def visit(stmts, armed):
        for st in stmts:
            bodies = [getattr(st, a) for a in ("body", "orelse", "finalbody")
                      if isinstance(getattr(st, a, None), list)]
            bodies += [h.body for h in getattr(st, "handlers", []) or []]
            if bodies and not isinstance(st, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                head = [getattr(st, a) for a in ("test", "iter") if getattr(st, a, None) is not None]
                head += [i.context_expr for i in getattr(st, "items", []) or []]
                inner = armed or any(_has_write(h) for h in head)
                for b in bodies:
                    visit(b, inner)
            elif armed:
                out.extend((x.lineno, _call_name(x)) for x in ast.walk(st)
                           if isinstance(x, ast.Call) and _call_name(x) in _HAND_BUILT)
            if _has_write(st) and not _exclusive_branch(st):
                armed = True

    visit(fn.body, False)
    return out


def _server_modules() -> dict:
    here = os.path.dirname(os.path.abspath(R.__file__))
    mods = {f: os.path.join(here, f) for f in sorted(os.listdir(here))
            if f.endswith(".py") and not f.startswith("test_")}
    assert "redesign.py" in mods and "factions.py" in mods, sorted(mods)   # fail OPEN
    return mods


def test_no_write_path_in_any_server_module_hand_builds_a_refusal():
    """L14 FF1 m-8: factions.py built its own blocks and ~20 why_blocked calls on
    redesign.py's write paths were never seen (the walker read redesign.py and
    banned map_raw_reason only). Every module, both builders."""
    hits, paths = [], 0
    for fname, path in _server_modules().items():
        tree = ast.parse(open(path, encoding="utf-8").read())
        for fn in ast.walk(tree):
            if not isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)) or fn.name in _BUILDERS:
                continue
            if _has_write(fn):
                paths += 1
            hits += [f"{fname}:{line} {fn.name} calls {call}" for line, call in
                     hand_built_after_a_write(fn)]
    assert paths >= 10, paths
    assert not hits, "\n".join(hits)


def test_the_walker_sees_a_hand_built_refusal_after_a_write():
    """Not a discriminating control: the rule fires on the shape it bans and not on
    an argument check before the write, nor across an exclusive branch."""
    src = (
        "async def f(gw, a):\n"
        "    if not a:\n"
        "        return why_blocked('missing_required_argument')\n"
        "    if a == 'x':\n"
        "        await gw.write('x', {})\n"
        "        return 1\n"
        "    if a == 'y':\n"
        "        return why_blocked('invalid_argument')\n"
        "    raw = await gw.write('y', {})\n"
        "    if not raw.get('success'):\n"
        "        return why_blocked('write_unconfirmed')\n")
    fn = ast.parse(src).body[0]
    assert [c for _, c in hand_built_after_a_write(fn)] == ["why_blocked"]
    assert hand_built_after_a_write(fn)[0][0] == 11


@pytest.mark.parametrize("path", sorted(DRIVERS))
def test_the_games_sentence_rides_in_blocked_detail(path):
    gw = Bridge()
    env = run(DRIVERS[path](gw))
    assert env["verdict"] == "rejected", (path, env)
    assert env["blocked"].get("detail") == WORDS, (path, env["blocked"])


@pytest.mark.parametrize("path", sorted(DRIVERS))
def test_the_game_said_nothing_is_said_and_an_unread_ring_is_not(path):
    env = run(DRIVERS[path](Bridge(ring_detail=None)))
    assert env["blocked"].get("detail") == R.NO_GAME_DETAIL, (path, env["blocked"])
    env = run(DRIVERS[path](Bridge(ring=False)))
    assert "detail" not in env["blocked"], (path, env["blocked"])


def test_the_detail_is_the_ring_rows_not_the_dispatch_results():
    """The drain read looks the intent up in the ring by id (newest wins)."""
    def ring(_p):
        return {"results": [{"intent_id": "other", "status": "rejected", "detail": "Not this"},
                            {"intent_id": RID, "status": "rejected", "detail": "Old words"},
                            {"intent_id": RID, "status": "rejected", "detail": "Is not adult"}]}
    gw = Bridge(replies={"query_intent_results": ring})
    env = run(DRIVERS["do_impl"](gw))
    assert env["blocked"]["detail"] == "Is not adult"
    assert env["blocked"]["summary"] == "send_conditions_not_met"   # the typed gate stays


def test_a_faction_drain_refusal_carries_the_words_too():
    raw = {"success": True, "status": "queued", "intent_request_id": RID}
    gw = Bridge(send_reply=raw)
    conf = run(R.settle_write(None, gw.read, raw, None, game_date_at_submit="867.1.5"))
    blk = factions._refusal(conf["refused"], "", None, drain=conf["drain"])
    assert blk["detail"] == WORDS
    assert factions._refusal("engine_refused_join", "X is a vassal")["detail"] == "X is a vassal"


TITLE_LAW = {"success": False, "action": "set_title_law", "intent_request_id": "stlaw_100_4_0",
             "dispatch_id": "stlaw_100_4_0", "reason": "engine_refused",
             "engine_reason": "\x16warning_icon! \x15X Missing \x15TOOLTIP:GAME_CONCEPT,prestige_i "
                              "\x16prestige_icon!\x15! 960\x15!"}


def _title_law(gw):
    return run(R.do_impl(gw, intent="change_law", character_id=ME,
                         args={"law": "feudal_elective_succession_law", "scope": "title",
                               "title_id": 998}))


def test_a_synchronous_engine_refusal_carries_its_words_and_the_pending_row_too():
    gw = Bridge(send_reply=TITLE_LAW)
    env = _title_law(gw)
    assert env["verdict"] == "rejected"
    assert env["blocked"]["detail"] == "Missing prestige 960", env["blocked"]
    # the inverse case: the ring row the bridge stored with no detail
    ring = {"results": [{"intent_id": "stlaw_100_4_0", "action_name": "set_title_law",
                         "status": "rejected", "rejection_reason": "engine_refused"}]}
    gw.replies["query_intent_results"] = ring
    gw.replies["query_intents"] = {"pending": []}
    pend = run(R._inspect_pending(gw, 16))
    assert pend["result"]["completed"][0]["detail"] == "Missing prestige 960"


def test_a_cost_only_refusal_is_the_resource_gate_and_retryable():
    """Minor 11: "Missing prestige 960" read retry "never"; prestige is earned."""
    env = _title_law(Bridge(send_reply=TITLE_LAW))
    b = env["blocked"]
    assert b["gate"] == "insufficient_prestige" and b["retryable"] is True, b
    assert b["retry"] != "never" and b["measured"] == {"missing": {"prestige": 960.0}}, b
    # the control: a refusal with any line that is not a price keeps the engine's gate
    mixed = dict(TITLE_LAW, engine_reason="Missing prestige 960\nIs not adult")
    b = _title_law(Bridge(send_reply=mixed))["blocked"]
    assert b["gate"] == "engine_refused" and b["retryable"] is False, b
    two = dict(TITLE_LAW, engine_reason="Missing prestige 960\nMissing gold 50")
    assert _title_law(Bridge(send_reply=two))["blocked"]["gate"] == "cannot_afford"


def test_an_argument_refusal_carries_no_game_detail():
    """A bridge code that is not the game's (no engine text, no drain) gains no detail."""
    gw = Bridge(send_reply={"success": False, "reason": "target_required"})
    env = run(R.do_impl(gw, intent="interact", character_id=ME,
                        args={"interaction": "gift_interaction", "target_id": FOE}))
    assert env["verdict"] == "rejected" and "detail" not in env["blocked"], env["blocked"]
