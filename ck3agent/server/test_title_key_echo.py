"""A title key is taken wherever a title id is, and echoed beside it."""
import asyncio

import redesign as R
import title_refs as T

DUBLIN = {"title": {"title_id": 11, "key": "c_dublin", "name": "Dublin", "tier": 2}}


class Gw:
    health = "ok"

    def __init__(self, known=None):
        self.known = {"c_dublin": DUBLIN} if known is None else known
        self.reads, self.writes = [], []

    async def read(self, action, params=None):
        p = dict(params or {})
        self.reads.append((action, p))
        if action == "query_de_jure":
            return self.known.get(p.get("title_key"), {"error": "title_key_not_found",
                                                       "title_key": p.get("title_key")})
        if action == "query_character":
            return {"id": 5, "alive": True, "name": "Diarmait"}
        return {}

    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return {"success": True}

    async def actor_gate(self):
        return None

    def game(self):
        return {"tick": 1, "date": "1066.9.15", "paused": True, "speed": 1}


def run(c):
    return asyncio.run(c)


def test_an_id_passes_through_unread_and_a_key_resolves_with_its_name():
    gw = Gw()
    assert run(T.resolve(gw, 11)) == (11, None, None) and gw.reads == []
    assert run(T.resolve(gw, "11")) == (11, None, None)
    tid, echo, blk = run(T.resolve(gw, "c_dublin", "target_title_id"))
    assert (tid, blk) == (11, None)
    assert echo == {"arg": "target_title_id", "title_key": "c_dublin", "title_id": 11, "name": "Dublin"}


def test_an_unknown_key_and_a_non_key_are_refused_and_nothing_is_sent():
    gw = Gw()
    _, _, blk = run(T.resolve(gw, "c_atlantis"))
    assert blk["gate"] == "target_not_assignable" and "c_atlantis" in blk["summary"]
    _, _, blk = run(T.resolve(gw, "Dublin"))
    assert blk["gate"] == "invalid_argument"
    _, _, blk = run(T.resolve(Gw(known={"c_dublin": {}}), "c_dublin"))
    assert blk["gate"] == "read_timeout"


def test_do_sends_the_id_for_a_key_and_names_it_in_the_reply():
    gw = Gw()
    env = run(R.do_impl(gw, intent="move_capital", character_id=5, args={"title_id": "c_dublin"}))
    sent = [p for a, p in gw.writes]
    assert sent and all(p.get("title_id") == 11 for p in sent if "title_id" in p), gw.writes
    assert env["result"]["titles_named"] == [{"arg": "title_id", "title_key": "c_dublin",
                                              "title_id": 11, "name": "Dublin"}]


def test_do_refuses_a_key_the_game_does_not_have_before_sending():
    gw = Gw()
    env = run(R.do_impl(gw, intent="council_task", character_id=5,
                        args={"task": "task_fabricate_claim", "target_title_id": "c_atlantis"}))
    assert env["ok"] is False and env["blocked"]["gate"] == "target_not_assignable"
    assert gw.writes == []


def test_declare_war_takes_cb_title_id_as_a_key_in_declare_args_and_flat():
    gw = Gw()
    env = run(R.declare_war_impl(gw, character_id=5, declare_args={"target_id": 9, "cb": "claim_cb",
                                                                    "cb_title_id": "c_atlantis"}))
    assert env["blocked"]["gate"] == "target_not_assignable" and gw.writes == []
    seen = {}

    async def fake(gw_, **kw):
        seen.update(kw)
        return R.envelope("declare_war", result={})
    orig = R._declare_war_impl
    R._declare_war_impl = fake
    try:
        env = run(R.declare_war_impl(gw, character_id=5, target_id=9, cb="claim_cb", cb_title_id="c_dublin"))
        assert seen["cb_title_id"] == 11
        assert env["result"]["titles_named"][0]["title_key"] == "c_dublin"
        run(R.declare_war_impl(gw, character_id=5, declare_args={"target_id": 9, "cb": "claim_cb",
                                                                  "cb_title_id": "c_dublin"}))
        assert seen["declare_args"]["cb_title_id"] == 11
    finally:
        R._declare_war_impl = orig


def test_inspect_takes_a_key_as_title_id():
    gw = Gw()
    env = run(R.inspect_impl(gw, subject="title_succession", character_id=5, title_id="c_dublin"))
    assert ("query_title_succession", {"character_id": 5, "title_id": 11}) in gw.reads
    assert env["result"]["titles_named"][0]["title_id"] == 11


# ── A marriage candidate row carries its do_call ─────────────────

def test_marriage_rows_carry_a_ready_do_call_and_say_alliance_is_not_read():
    reply = {"actor_id": 5, "candidates": [{"id": 77, "name": "Gormlaith", "is_ruler": True, "age": 30}]}
    people = {5: {"id": 5, "name": "Me", "is_female": False, "age": 30, "spouses": []},
              77: {"id": 77, "name": "Gormlaith", "is_female": True, "age": 30, "spouses": []}}

    async def read(_a, p):
        return people.get(p["character_id"], {})
    out = run(R.marriage_rows(reply, read))
    (row,) = out["candidates"]
    assert row["do_call"] == {"tool": "do", "args": {"intent": "interact", "args": {
        "interaction": "arrange_marriage_interaction", "target_id": 77,
        "secondary_actor_id": 5, "secondary_recipient_id": 77}}}
    assert out["alliance"] == R.MARRIAGE_ALLIANCE_RULE      # The rule; the readback reads it
    assert run(R.marriage_rows({"error": "x"}, read)) == {"error": "x"}


# ── The income levers are real intents with their real enumerators ──

def test_income_levers_name_only_intents_classes_and_args_the_code_has():
    import do_intent_args
    table = do_intent_args.extract(do_intent_args.REDESIGN.read_text(encoding="utf-8"))
    table.update(do_intent_args.extract_dedicated())
    for row in R.INCOME_LEVERS + R.INCOME_NOT_LEVERS + (R.hof_gold_lever(None),):
        dc = row["do_call"]
        assert dc["intent"] in R.INTENTS, row
        assert set(dc["args"]) <= set(table[dc["intent"]]), row
        en = row.get("enumerate")
        if en and "action_class" in en["args"]:
            assert en["args"]["action_class"] in R.ACTION_CLASSES, row
        if en and en["tool"] == "inspect":
            assert en["args"]["subject"] in R.INSPECT_SUBJECTS
    env = run(R.do_impl(Gw(), action_class="income_levers", character_id=5))
    # The head of faith's gold, priced per ruler, after the static rows
    assert [r["lever"] for r in env["result"]["available"]] == [r["lever"] for r in R.INCOME_LEVERS] + ["hof_gold"]
