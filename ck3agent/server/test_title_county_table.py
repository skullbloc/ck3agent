"""inspect(subject="title") is one table of a de-jure title's counties.

The fixture is a two-duchy kingdom shaped like the bridge's replies (query_handlers.cpp
do_query_dejure_progress verbose rows, emit_title_info, query_realm_batch, do_query_claims).
"""
import asyncio

import redesign as R

ME = 100          # the caller, holds c_dublin; vassal 101 holds c_fingal inside my realm
KING = 200        # another realm's ruler: c_meath, c_breifne (vassal 201 holds c_breifne)


def _fake_replies():
    return {
        ("query_dejure_progress",): {"character_id": ME, "title_key": "k_ireland", "title_id": 50,
                                     "held": 2, "total": 4, "fraction": 0.5, "counties": [
            {"county_id": 11, "holder_id": ME, "top_liege_id": ME, "you_control": True},
            {"county_id": 12, "holder_id": 101, "top_liege_id": ME, "you_control": True},
            {"county_id": 21, "holder_id": KING, "top_liege_id": KING, "you_control": False},
            {"county_id": 22, "holder_id": 201, "top_liege_id": KING, "you_control": False}]},
        ("query_de_jure", "k_ireland"): {"title": {"title_id": 50, "key": "k_ireland", "name": "Ireland",
                                                   "tier": 4, "tier_name": "kingdom"},
                                         "de_jure_children": [
            {"title_id": 1, "key": "d_leinster", "name": "Leinster", "tier": 3, "holder_id": ME,
             "holder_name": "Diarmait"},
            {"title_id": 2, "key": "d_meath", "name": "Meath", "tier": 3, "holder_id": KING,
             "holder_name": "Conchobar"}]},
        ("query_de_jure", 1): {"title": {"title_id": 1}, "de_jure_children": [
            {"title_id": 11, "key": "c_dublin", "name": "Dublin", "tier": 2, "holder_id": ME,
             "holder_name": "Diarmait"},
            {"title_id": 12, "key": "c_fingal", "name": "Fingal", "tier": 2, "holder_id": 101,
             "holder_name": "Sitric"}]},
        ("query_de_jure", 2): {"title": {"title_id": 2}, "de_jure_children": [
            {"title_id": 21, "key": "c_meath", "name": "Meath", "tier": 2, "holder_id": KING,
             "holder_name": "Conchobar"},
            {"title_id": 22, "key": "c_breifne", "name": "Breifne", "tier": 2, "holder_id": 201,
             "holder_name": "Ruarc"}]},
        ("query_claims",): {"character_id": ME, "claims": [
            {"title_id": 21, "title_name": "Meath", "pressed": True, "type": "pressed"}]},
        ("query_realm",): {"realms": [
            {"character_id": ME, "military_strength": 900, "realm_total_counties": 3,
             "primary_title_key": "d_leinster"},
            {"character_id": KING, "military_strength": 2500, "realm_total_counties": 9,
             "primary_title_key": "d_meath"}]},
        ("query_character",): {"character_id": 0, "name": "?"},
    }


class Gw:
    health = "ok"

    def __init__(self, replies=None):
        self.replies = replies or _fake_replies()
        self.reads = []

    async def read(self, action, params=None):
        p = dict(params or {})
        self.reads.append((action, p))
        if action == "query_de_jure":
            return self.replies.get((action, p.get("title_key") or p.get("title_id")), {})
        return self.replies.get((action,), {})

    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def actor_gate(self):
        return None

    def game(self):
        return {"tick": 1, "date": "1066.9.15", "paused": True, "speed": 1}


def run(c):
    return asyncio.run(c)


def test_each_county_row_carries_key_name_holder_realm_strength_control_and_claim():
    env = run(R.inspect_impl(Gw(), subject="title", character_id=ME, title_key="k_ireland"))
    assert env["ok"] is True, env
    res = env["result"]
    import title_table as TT
    rows = {r["key"]: r for r in res["counties"]}
    assert set(rows) == {"c_dublin", "c_fingal", "c_meath", "c_breifne"}
    m = dict(rows["c_meath"])
    # next_call is test_ireland_walk_replies.py's subject (this Gw serves no cbs read)
    assert TT.next_call_of(res, m)["gate"] == "read_timeout"
    m.pop("next_call")
    # The realm ruler's name and strength are in realms, by ruler_id (title_table.seat_rows)
    realm = next(r for r in res["realms"] if r["ruler_id"] == m["realm_ruler_id"])
    assert (realm["ruler_name"], realm["military_strength"]) == ("Conchobar", 2500)
    assert m == {"title_id": 21, "key": "c_meath", "name": "Meath", "duchy": "d_meath",
                 "holder_id": KING, "holder_name": "Conchobar", "realm_ruler_id": KING,
                 "you_control": False, "your_claim": "pressed"}
    assert rows["c_breifne"]["holder_name"] == "Ruarc" and rows["c_breifne"]["realm_ruler_id"] == KING
    assert rows["c_fingal"]["you_control"] is True and rows["c_fingal"]["your_claim"] is False


def test_duchies_realms_and_the_creation_bar_are_summarised():
    res = run(R.inspect_impl(Gw(), subject="title", character_id=ME, title_key="k_ireland"))["result"]
    s = res["summary"]
    assert s == {"counties_you_control": 2, "counties": 4, "fraction": 0.5, "creation_needs_counties": 3,
                 "duchies_you_hold": 1, "duchies": 2, "held": 2, "total": 4}   # More than half of 4
    d = {x["key"]: x for x in res["duchies"]}
    assert d["d_leinster"]["you_hold_it"] is True and d["d_leinster"]["counties_you_control"] == 2
    assert d["d_meath"]["you_hold_it"] is False and d["d_meath"]["counties"] == 2
    assert [r["ruler_id"] for r in res["realms"]] in ([ME, KING], [KING, ME])
    assert {r["ruler_id"]: r["military_strength"] for r in res["realms"]} == {ME: 900, KING: 2500}
    assert res["creation_rule"] == R.MINT_DEJURE_RULE


def test_an_unread_claims_list_is_null_not_no_claim():
    rep = _fake_replies()
    rep[("query_claims",)] = {}
    env = run(R.inspect_impl(Gw(rep), subject="title", character_id=ME, title_key="k_ireland"))
    assert all(r["your_claim"] is None for r in env["result"]["counties"])
    assert env["ok"] is False and any("query_claims" in e["detail"] for e in env["errors"])


def test_a_title_not_named_is_refused_and_an_unknown_key_says_so():
    assert R.inspect_impl  # noqa
    env = run(R.inspect_impl(Gw(), subject="title", character_id=ME))
    assert env["blocked"]["gate"] == "missing_required_argument"
    rep = _fake_replies()
    rep[("query_dejure_progress",)] = {"title_id": -1, "error": "title_key_not_found"}
    env = run(R.inspect_impl(Gw(rep), subject="title", character_id=ME, title_key="k_nowhere"))
    assert env["blocked"]["gate"] == R.why_blocked("title_key_not_found")["gate"]
    assert "k_nowhere" in env["blocked"]["summary"]
