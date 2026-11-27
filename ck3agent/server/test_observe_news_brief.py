"""Items D and H: observe's `news` (the diff between two observes) and the brief's
military, liege, council and money unit."""
import asyncio

import news
import redesign as R

ME, LIEGE = 100, 300


class Gw:
    health = "ok"

    def __init__(self):
        self.date = "1066.9.15"
        self.claims = []
        self.wars = []
        self.task = "task_domestic_affairs"
        self.history = {}
        self.reads = []

    async def read(self, action, params=None):
        p = dict(params or {})
        self.reads.append((action, p))
        if action == "query_character":
            if p.get("character_id") == LIEGE:
                return {"id": LIEGE, "name": "Toirdelbach", "alive": True}
            return {"id": ME, "name": "Diarmait", "alive": True, "gold": 120.5, "age": 40,
                    "liege_id": LIEGE, "top_liege_id": LIEGE,
                    "income_breakdown": {"total_monthly": 3.25}}
        if action == "query_claims":
            return {"character_id": ME, "claims": list(self.claims)}
        if action == "get_active_wars":
            return {"wars": list(self.wars)}
        if action == "query_council":
            return {"council": [{"position_key": "councillor_chancellor", "holder_id": 7, "vacant": False,
                                 "current_task": self.task}]}
        if action == "query_levy_readiness":
            return {"military_current": 1450, "military_max": 1800, "levy_own_domain_max": 900,
                    "regiments": [{"type": "levy", "current": 700}, {"type": "armored_footmen", "current": 100},
                                  {"type": "light_horsemen", "current": 50}]}
        if action == "query_war_history":
            return self.history.get(p.get("war_id"), {})
        if action == "query_realm":
            return {"character_id": ME, "vassals": [], "held_title_count": 1}
        return {}

    async def read_many(self, specs):
        return [await self.read(a, p) for a, p in specs]

    async def actor_gate(self):
        return None

    async def delta_records_since_last(self):
        return []

    def game(self):
        return {"tick": 1, "date": self.date, "paused": True, "speed": 1}


def run(c):
    return asyncio.run(c)


def _observe(gw):
    env = run(R.observe_impl(gw, character_id=ME))
    assert isinstance(env["result"], dict), env
    return env["result"]


def test_the_first_observe_says_it_has_nothing_to_compare():
    news.reset()
    n = _observe(Gw())["news"]
    assert n["first_observe"] is True and n["since"] is None


def test_a_claim_gained_a_war_started_and_ended_and_a_council_change_are_news():
    news.reset()
    gw = Gw()
    gw.wars = [{"war_id": 4, "name": "Old war", "attacker_id": ME, "defender_id": 9, "is_attacker": True}]
    _observe(gw)
    gw.date = "1066.10.15"
    gw.claims = [{"title_id": 21, "title_name": "Meath", "type": "unpressed"}]
    gw.wars = [{"war_id": 8, "name": "Conquest of Meath", "attacker_id": ME, "defender_id": 200,
                "is_attacker": True}]
    gw.history = {4: {"wars": [{"war_id": 4, "outcome": "white_peace", "end_date": "1066.10.2"}]}}
    gw.task = "task_fabricate_claim"
    n = _observe(gw)["news"]
    assert n["since"] == "1066.9.15"
    # A claim row with no holder read keeps plan_war (test_ireland_walk_replies.py: declare_args)
    assert n["claims_gained"] == [{"title_id": 21, "title_name": "Meath", "type": "unpressed",
                                   "holder_id": None, "next": {"tool": "plan_war", "args": {}},
                                   # An unpressed claim dies with the ruler
                                   "on_your_death": news.CLAIM_DIES,
                                   # Its kind, the ruler's age, the war's state
                                   "kind": dict(news.CLAIM_KIND["unpressed"], type="unpressed"),
                                   "holder_is_your_ruler": {"age": 40},
                                   "war": {"declarable_now": None, "unread": "the claim's holder was not read"}}]
    assert n["wars_started"][0]["war_id"] == 8 and n["wars_started"][0]["my_side"] == "attacker"
    assert n["wars_started"][0]["next"] == {"tool": "inspect", "args": {"subject": "war_score", "war_id": 8}}
    (ended,) = n["wars_ended"]
    assert (ended["war_id"], ended["outcome"], ended["end_date"]) == (4, "white_peace", "1066.10.2")
    (c,) = n["council_changed"]
    assert c["seat"] == "councillor_chancellor" and c["now"]["task"] == "task_fabricate_claim"


def test_an_unread_section_is_unread_not_no_change():
    news.reset()
    gw = Gw()
    _observe(gw)
    orig = gw.read

    async def no_claims(action, params=None):
        return {} if action == "query_claims" else await orig(action, params)
    gw.read = no_claims
    n = _observe(gw)["news"]
    assert "claims_gained" not in n and "claims" in n["unread"]["sections"]


def test_a_war_the_ledger_does_not_list_has_no_invented_outcome():
    news.reset()
    gw = Gw()
    gw.wars = [{"war_id": 4, "name": "Old war", "attacker_id": ME, "defender_id": 9, "is_attacker": False}]
    _observe(gw)
    gw.wars = []
    (ended,) = _observe(gw)["news"]["wars_ended"]
    assert ended["outcome"] is None and ended["my_side"] == "defender" and ended["outcome_unread"]


def test_the_brief_carries_military_liege_council_and_the_money_unit():
    news.reset()
    res = _observe(Gw())
    assert res["military"]["strength"] == 1450 and res["military"]["men_at_arms_men"] == 150
    assert res["military"]["men_at_arms_regiments"] == 2 and "head count" in res["military"]["unit"]
    assert res["liege"] == {"id": LIEGE, "independent": False, "name": "Toirdelbach"}
    assert res["council"] == [{"seat": "councillor_chancellor", "holder_id": 7, "vacant": False,
                               "task": "task_domestic_affairs"}]
    assert res["money"]["monthly_income"] == 3.25 and "per game month" in res["money"]["unit"]
