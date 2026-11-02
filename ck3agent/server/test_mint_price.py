"""L14 S3 (surface-eval finding 14): a mint the player cannot pay for is refused,
and an applied mint carries one verdict.

The field case: Murchad of Munster (31762, 1066 save) called
do(intent="mint_title", args={"title_key": "k_ireland"}) at 1071.10.15; the kingdom
was created and the reply said verdict "applied" beside raw.success false. His gold
was read 17.85 at 1071.06.23 (the last reading before the mint) and -302.21 at
1072.09.22; no reading exists at the mint itself (docs/cleanup-2026.11/evidence/l14/
murchad-1066.md). 320.06 below is a FIXTURE VALUE, not a measured price: it is
17.85 - (-302.21), the difference of two readings 15 months apart ("about 320,
back-solved, not measured"). The rows only need a price above the gold held. The
price block is query_create_title_cost's reply shape (bridge/src/action_handlers.cpp
do_query_create_title_cost).

Driven through redesign.do_impl with a scripted gateway, so each row reds on what
do() SENDS and RETURNS.
"""
import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")

MURCHAD = 31762
KEY = "k_ireland"
#: Murchad's realm at the mint: 8 of 14 de-jure counties (the formable_titles row).
DEJURE = {"character_id": MURCHAD, "title_key": KEY, "title_id": 461,
          "held": 8, "total": 14, "fraction": 0.571}
#: The bridge's first reply to that create_title: the transcript's ids and date, in the
#: shape the bridge sends today (status queued, no_poll_on_game_thread). The field reply
#: said dispatched_pending / not_confirmed_within_12s, a poll the bridge no longer runs
#: and a reason it no longer sends (L14 FB m-5).
MURCHAD_RAW = {"request_id": "32f3bf08-6a6e-43ba-892b-c8662bdf3f0a", "tick": 1855,
               "game_date": "1071.10.15", "success": False, "action": "create_title",
               "actor_id": MURCHAD, "title_id": 461, "title_key": KEY,
               "intent_request_id": "crtitle_31762_1855_36297",
               "dispatch_id": "crtitle_31762_1855_36297", "status": "queued",
               "reason": "no_poll_on_game_thread", "drains_when": "next_pass"}


def price_reply(cost=320.06, have=17.85, *, engine_valid=None, known=True, success=True,
                currency="gold", reason="", reasons=()):
    if not success:
        return {"success": False, "action": "query_create_title_cost", "reason": reason}
    price = ({"known": True, "currency": currency, "cost": cost, "have": have,
              "affordable": have >= cost} if known else {"known": False, "reason": reason})
    if engine_valid is None:
        engine_valid = have >= cost
    return {"success": True, "action": "query_create_title_cost", "actor_id": MURCHAD,
            "title_id": 461, "title_key": KEY, "engine_valid": engine_valid,
            "engine_reasons": list(reasons), "price": price}


class Gateway:
    """Answers each read from a script; records writes; confirmed_write runs the
    write and then the readback, as confirm_write does when the drain ran it."""

    def __init__(self, price, *, held_after=False, raw=None, dejure=None, primary_after=None):
        self.price = price
        self.held_after = held_after
        # the realm's primary title after the write: Murchad's new kingdom by default
        # (the game promotes a higher tier), else what the test says it stays
        self.primary_after = primary_after or (KEY if held_after else "d_munster")
        self.title_row = True                   # None: query_title_succession answers {}
        self.raw = MURCHAD_RAW if raw is None else raw
        self.dejure = DEJURE if dejure is None else dejure
        self.reads: list = []
        self.writes: list = []
        self.health = "ok"

    async def actor_gate(self):
        return None

    def game(self):
        return {"tick": 1855, "date": "1071.10.15", "paused": True, "speed": 3}

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"id": MURCHAD, "alive": True, "name": "Murchad"}
        if action == "query_dejure_progress":
            return dict(self.dejure)
        if action == "query_create_title_cost":
            return self.price
        if action == "query_realm":
            # query_realm (query_handlers.cpp ~1500): primary_title_key and counts,
            # NO titles list. Murchad's kingdom becomes his primary title; a king's
            # new duchy does not (self.primary_after).
            return {"primary_title_key": self.primary_after, "character_id": MURCHAD,
                    "held_title_count": 2, "capital_barony_id": 1}
        if action == "query_dispatch_result":
            # the drain ran the intent (the completed ring's row, the shape
            # test_war_intents._dispatch_by_id answers): settle_write's stage 1 "ran"
            return {"dispatch_id": (params or {}).get("dispatch_id"), "status": "success",
                    "action_name": "create_title"}
        if action == "query_title_succession":
            if self.title_row is None:
                return {}                      # the read did not come back
            # the title's own row (query_handlers.cpp do_query_title_succession,
            # current_holder_id at 12126): the shape of
            # l14h-geval-p eval_b_play step inspect(title_succession, 430)
            tid = int((params or {}).get("title_id") or 0)
            return {"action": "query_title_succession", "title_id": tid, "title_key": KEY,
                    "tier": 4, "current_holder_id": MURCHAD if self.held_after else 0,
                    "current_holder_name": "Murchad", "succession_order": "inheritance",
                    "title_laws": [], "heir_count": 0, "heirs": []}
        return {}

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return dict(self.raw)

    async def confirmed_write(self, action, params, *, readback, deadline_s=12.0):
        raw = await self.write(action, params)
        resolved = await readback(raw)
        return {"raw": raw, "resolved": resolved, "refused": "", "sent": True,
                "drains_when": "next_pass", "game_date_at_submit": "1071.10.15",
                "intent_id": raw.get("intent_request_id", "")}


def _mint(gw):
    return asyncio.run(redesign.do_impl(gw, intent="mint_title", character_id=MURCHAD,
                                        args={"title_key": KEY}))


def _formable(gw):
    return asyncio.run(redesign.do_impl(gw, action_class="formable_titles",
                                        character_id=MURCHAD, candidates=[KEY]))


def test_murchads_mint_is_refused_cannot_afford_with_the_price():
    """17.85 gold against a 320.06 price: rejected cannot_afford, the game's price
    and his gold in `measured`, and no create_title is sent."""
    gw = Gateway(price_reply())
    env = _mint(gw)
    assert env["ok"] is False and env["verdict"] == "rejected", env
    blk = env["blocked"]
    assert blk["gate"] == "cannot_afford", blk
    assert blk["measured"] == {"currency": "gold", "cost": 320.06, "gold": 17.85}, blk
    assert "320.06" in blk["summary"] and "17.85" in blk["summary"], blk["summary"]
    assert not [a for a, _ in gw.writes if a == "create_title"], gw.writes
    assert ("query_create_title_cost", {"actor_id": MURCHAD, "title_key": KEY}) in gw.reads


def test_formable_titles_lists_the_unaffordable_title_unavailable():
    gw = Gateway(price_reply())
    env = _formable(gw)
    res = env["result"]
    assert not res["available"], res
    (row,) = res["unavailable"]
    assert row["blocked"]["gate"] == "cannot_afford", row
    assert row["price"] == {"currency": "gold", "cost": 320.06, "gold": 17.85}, row
    detail = " ".join(w.get("detail", "") for w in env["warnings"])
    assert "only gold + not" not in detail and "bypasses the #131 bar" not in detail, detail


def test_an_unread_price_refuses_the_mint_mint_cost_unverified():
    """Neither the price nor the engine's verdict read: refused, never sent."""
    gw = Gateway(price_reply(success=False, reason="rng_scope_latched"))
    env = _mint(gw)
    assert env["blocked"]["gate"] == "mint_cost_unverified", env["blocked"]
    assert env["blocked"]["raw_reason"] == "rng_scope_latched"
    assert not gw.writes


def test_unmapped_price_functions_fall_back_to_the_engine_verdict():
    """The Windows build has no price functions mapped: the engine's IsValid decides
    (a yes sends the mint, a no refuses with the game's reasons)."""
    gw = Gateway(price_reply(known=False, reason="price_fn_unmapped", engine_valid=True),
                 held_after=True)
    assert _mint(gw)["verdict"] == "applied"
    assert [a for a, _ in gw.writes] == ["create_title"]
    gw = Gateway(price_reply(known=False, reason="price_fn_unmapped", engine_valid=False,
                             reasons=["You do not control enough counties"]))
    env = _mint(gw)
    assert env["blocked"]["gate"] == "create_title_engine_gate", env["blocked"]
    assert "control enough counties" in env["blocked"]["summary"]
    assert not gw.writes


def test_affordable_but_refused_by_the_game_is_the_engine_gate():
    gw = Gateway(price_reply(cost=250.0, have=400.0, engine_valid=False,
                             reasons=["Title already exists"]))
    env = _mint(gw)
    assert env["blocked"]["gate"] == "create_title_engine_gate", env["blocked"]
    assert env["blocked"]["measured"]["cost"] == 250.0
    assert not gw.writes


def test_an_applied_mint_carries_one_polarity():
    """Murchad's reply shape with the price paid: applied with the readback, and the
    bridge's interim success:false is not carried beside it."""
    gw = Gateway(price_reply(cost=320.06, have=400.0), held_after=True)
    env = _mint(gw)
    assert env["verdict"] == "applied" and env["ok"] is True, env
    res = env["result"]
    assert res["readback"]["title_held"] == KEY
    raw = res.get("raw")
    assert raw is None or raw.get("success") is True, res
    assert res["request_id"] == "crtitle_31762_1855_36297"


def test_applied_result_drops_only_a_raw_that_did_not_say_success():
    """The do() tail's applied verdict goes through the same function."""
    keep = {"intent": "x", "raw": {"success": True, "status": "applied"}}
    assert redesign.applied_result(keep) == keep
    out = redesign.applied_result({"intent": "x", "raw": {"success": False,
                                                          "status": "queued",
                                                          "request_id": "r1"}})
    assert "raw" not in out and out["request_id"] == "r1"
    out = redesign.applied_result({"intent": "x", "raw": {"status": "pending",
                                                          "intent_request_id": "i9"}})
    assert "raw" not in out and out["request_id"] == "i9"


def test_a_refused_mint_by_a_ruler_with_full_dejure_control_is_not_applied():
    """L14 FF1 B-1 (the witness's run 1): the game refuses the mint of a ruler who
    holds every de-jure county. Full de-jure control is the mint's precondition,
    not its product, so it never reads back as applied: the reply's own refusal
    is the verdict."""
    gw = Gateway(price_reply(cost=10.0, have=400.0),
                 raw={"success": False, "reason": "engine_refused"},
                 dejure=dict(DEJURE, held=5, total=5, fraction=1.0))
    env = _mint(gw)
    assert env["verdict"] != "applied", env
    assert env["verdict"] == "rejected" and env["ok"] is False, env
    assert env["blocked"]["raw_reason"] == "engine_refused", env["blocked"]


#: The bridge's fault reply, in the shape it really sends: MEMORY_ACCESS_FAULT_REPLY
#: (bridge/include/bridge.h), answered by answer_memory_access_fault and by the
#: SIGSEGV recovery in ipc_server.cpp serve_on_game_thread. No `success` key.
FAULT = {"error": "memory_access_fault"}


def test_a_faulted_mint_with_the_title_in_the_realm_is_not_applied():
    """L14 FF2 N-MAJ-1 (the witness's run 3): create_title answers the fault and
    query_realm lists the key. The reply's own refusal outranks the readback.
    RED on b84e3634 (applied)."""
    gw = Gateway(price_reply(cost=10.0, have=400.0), raw=dict(FAULT), held_after=True)
    env = _mint(gw)
    assert env["verdict"] != "applied", env
    assert env["verdict"] == "rejected" and env["ok"] is False, env
    assert env["blocked"]["raw_reason"] == "memory_access_fault", env["blocked"]


class RealSettle(Gateway):
    """confirmed_write through the real confirm_write -> settle_write, so the
    rule under test is the module's, not this double's."""

    async def clock(self):
        return self.game()

    async def confirmed_write(self, action, params, *, readback, deadline_s=12.0):
        return await redesign.confirm_write(self.clock, self.read,
                                            lambda: self.write(action, params), readback,
                                            deadline_s=deadline_s, max_polls=0)


@pytest.mark.parametrize("raw", [FAULT, {"reason": "bridge_disconnected"}])
def test_settle_write_refuses_a_fault_before_the_readback(raw):
    """The same through settle_write: a reply that queued nothing and carries the
    bridge's fault (or the hung gateway's bridge_disconnected) is refused before
    any readback can resolve on the title already in the realm. RED on b84e3634."""
    gw = RealSettle(price_reply(cost=10.0, have=400.0), raw=dict(raw), held_after=True)
    env = _mint(gw)
    assert env["verdict"] == "rejected" and env["ok"] is False, env
    assert env["blocked"]["raw_reason"] == (raw.get("error") or raw.get("reason")), env["blocked"]


def test_a_mint_not_held_after_the_pass_is_the_levers_absent_outcome():
    """Sent, not refused, and the title is not in the realm after the pass: the
    LEVERS row for mint_title says what that is (absent "unconfirmed" ->
    rejected write_unconfirmed), whatever de-jure control reads."""
    assert redesign.lever_of("mint_title")[1]["absent"] == "unconfirmed"
    gw = Gateway(price_reply(cost=10.0, have=400.0),
                 dejure=dict(DEJURE, held=5, total=5, fraction=1.0))
    env = _mint(gw)
    assert env["verdict"] == "rejected", env
    assert env["blocked"]["gate"] == "write_unconfirmed", env["blocked"]


def test_control_the_title_held_after_the_pass_is_applied():
    """The control for the two rows above: the title's holder is the ruler -> applied."""
    gw = Gateway(price_reply(cost=10.0, have=400.0), held_after=True,
                 dejure=dict(DEJURE, held=5, total=5, fraction=1.0))
    env = _mint(gw)
    assert env["verdict"] == "applied", env
    assert env["result"]["readback"] == {"title_held": KEY, "title_id": 461, "via": "title_holder"}
    assert ("query_title_succession", {"title_id": 461}) in gw.reads, gw.reads


def test_a_kings_new_duchy_is_read_off_the_title_not_the_realms_primary():
    """l14h geval (2026-10-31), both clock modes: Lothaire II (a king) minted d_bar;
    the drain ran it and his gold fell by the price, but the readback asked
    query_realm for a primary title or a titles list, and a king's primary stays
    his kingdom while query_realm has no titles list, so a title the game created
    was answered "rejected" (write_unconfirmed). The holder is read off the title
    the create_title reply names. RED on d6290865."""
    gw = Gateway(price_reply(cost=212.5, have=321.0), held_after=True,
                 primary_after="k_lotharingia",
                 dejure=dict(DEJURE, held=3, total=3, fraction=1.0))
    env = _mint(gw)
    assert env["verdict"] == "applied" and env["ok"] is True, env
    assert env["result"]["readback"]["via"] == "title_holder", env["result"]
    assert "raw" not in env["result"], env["result"]
    # and the title held by someone else is still not applied
    gw = Gateway(price_reply(cost=212.5, have=321.0), held_after=False,
                 primary_after="k_lotharingia",
                 dejure=dict(DEJURE, held=3, total=3, fraction=1.0))
    env = _mint(gw)
    assert env["verdict"] == "rejected" and env["blocked"]["gate"] == "write_unconfirmed", env


def test_the_51_percent_rule_is_one_statement_and_mirrors_the_bridge():
    """L14 FF1 m-2: the warning, the formable_titles rows and the mint pre-check
    apply the #131 bar at the bridge's tiers (>= 4) and fraction, read from one
    place; the bridge's constants are checked here."""
    import re
    src = open(os.path.join(os.path.dirname(__file__), "..", "..", "bridge", "src",
                            "intent_dispatch.cpp"), encoding="utf-8").read()
    tier = re.search(r"CREATE_TITLE_DEJURE_MIN_TIER\s*=\s*(\d+)", src)
    frac = re.search(r"CREATE_TITLE_DEJURE_MIN_FRAC\s*=\s*([\d.]+)", src)
    assert tier and frac, "the bridge's #131 constants moved"
    assert int(tier.group(1)) == redesign.MINT_DEJURE_MIN_TIER
    assert float(frac.group(1)) == redesign.MINT_DEJURE_MIN_FRAC
    assert [redesign.title_tier_of_key(k) for k in ("b_x", "c_x", "d_x", "k_x", "e_x", "x")] == \
        [1, 2, 3, 4, 5, None]


def test_a_duchy_short_of_51_percent_is_the_games_call_not_the_servers():
    """A duchy at 1 of 4 de-jure counties: no de_jure_control pre-refusal (the
    bridge's bar is tier >= 4); the game's own IsValid and price decide."""
    duchy = dict(DEJURE, title_key="d_munster", held=1, total=4, fraction=0.25)
    gw = Gateway(price_reply(cost=10.0, have=400.0), dejure=duchy)
    env = asyncio.run(redesign.do_impl(gw, intent="mint_title", character_id=MURCHAD,
                                       args={"title_key": "d_munster"}))
    assert (env.get("blocked") or {}).get("gate") != "de_jure_control", env
    assert [a for a, _ in gw.writes] == ["create_title"], gw.writes
    gw = Gateway(price_reply(cost=10.0, have=400.0), dejure=duchy)
    env = asyncio.run(redesign.do_impl(gw, action_class="formable_titles",
                                       character_id=MURCHAD, candidates=["d_munster"]))
    assert env["result"]["available"], env["result"]


def test_control_a_kingdom_short_of_51_percent_is_refused_before_sending():
    king = dict(DEJURE, held=5, total=14, fraction=0.357)
    gw = Gateway(price_reply(cost=10.0, have=400.0), dejure=king)
    env = _mint(gw)
    assert env["blocked"]["gate"] == "de_jure_control", env["blocked"]
    assert env["blocked"]["measured"]["need"] == 8 and not gw.writes


def test_an_unpriced_duchy_is_refused_not_left_to_isvalid():
    """L14 FF1 m-3: the game's check skips the cost leg for a duchy in one case,
    so an unread price with IsValid yes is mint_cost_unverified for a duchy; a
    kingdom (the control) still falls back to IsValid."""
    duchy = dict(DEJURE, title_key="d_munster", held=4, total=4, fraction=1.0)
    gw = Gateway(price_reply(known=False, reason="price_fn_unmapped", engine_valid=True),
                 dejure=duchy)
    env = asyncio.run(redesign.do_impl(gw, intent="mint_title", character_id=MURCHAD,
                                       args={"title_key": "d_munster"}))
    assert env["blocked"]["gate"] == "mint_cost_unverified", env["blocked"]
    assert not gw.writes
    gw = Gateway(price_reply(known=False, reason="price_fn_unmapped", engine_valid=True),
                 held_after=True)
    assert _mint(gw)["verdict"] == "applied"


def test_the_levers_header_names_every_synchronous_row():
    """L14 FF1 m-9: the header said synchronous handlers are not rows while
    ("steer", "") is one; the text names it."""
    import inspect
    src = inspect.getsource(redesign)
    head = src[src.index("#: THE LEVER TABLE"):src.index("LEVERS: dict[tuple, dict] = {")]
    assert '("steer", "")' in head, head


def test_a_title_read_that_did_not_come_back_after_a_drained_mint_is_pending_readback():
    """Witness r4 N4-2 (R1): the drain ran the mint and query_title_succession
    answered nothing. That is a reading not made, not "not held": through the
    module's own settle_write the verdict is pending, drains_when "readback",
    never rejected write_unconfirmed. RED on 9c8d5581 (rejected)."""
    gw = RealSettle(price_reply(cost=212.5, have=321.0), held_after=True, primary_after="k_lotharingia",
                    dejure=dict(DEJURE, held=3, total=3, fraction=1.0))
    gw.title_row = None
    env = _mint(gw)
    assert env["verdict"] == "pending", env
    assert env["pending"]["drains_when"] == "readback", env["pending"]
    assert "title_succession" in env["pending"]["verify_with"], env["pending"]
    assert "blocked" not in env or not env["blocked"], env
    # a reply that names no title gives the readback nothing to read: the same
    gw = RealSettle(price_reply(cost=212.5, have=321.0), held_after=True,
                    raw={k: v for k, v in MURCHAD_RAW.items() if k != "title_id"},
                    dejure=dict(DEJURE, held=5, total=5, fraction=1.0))
    env = _mint(gw)
    assert env["verdict"] == "pending" and env["pending"]["drains_when"] == "readback", env
    assert not any(a == "query_realm" for a, _ in gw.reads), "the realm's primary title is not a mint readback"
