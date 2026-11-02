"""ck-m3xd — the money block reports what is real and says what is missing.

HOST ONLY. Pure shape over a reply; no bridge, no game.

MEASURED over d45-d50, 1,983 observations carrying `income_breakdown`:
  1,952 (98.4%)  EVERY component zero -- gross_domain, net_domain,
                 total_expenses, net_gold -- while total_monthly carried a real
                 number. A breakdown that breaks nothing down, and four zeros
                 that read as "no expenses" rather than "not populated".
     761 (38%)   income_breakdown.total_monthly DISAGREED with monthly_income.
                 L15 A3: they were never one quantity. query_character's
                 `monthly_income` was core_data+0x170, the MERIT stock (loc key
                 TOTAL_MERIT); the bridge now emits it as `merit`, and the money's
                 monthly_income is income_breakdown.total_monthly.

THE LIMIT THIS BEAD DOES NOT EXCEED. The design asked for "income/expense lines
that sum". THERE IS NO EXPENSE SIDE: total_expenses is 0 in all 1,983. So the
reconciliation cannot be computed and is not faked -- `expenses` is None with a
reason. Deriving an expense line would be a fabricated number on a screen the
seat is meant to trust.

  M1  gold and monthly_income (= income_breakdown.total_monthly) are reported
  M2  THE 98.4% CASE: an all-zero breakdown is reported ABSENT with a reason,
      never passed through as zeros
  M3  a partially populated breakdown reports the populated keys AND names the
      ones still missing
  M4  L15 A3: monthly_income is income_breakdown.total_monthly, never the merit;
      there is no second total and no "totals disagree" note
  M5  no breakdown, no income: monthly_income None (unmeasured), never 0 or merit
  M6  `expenses` is None with a reason -- never 0, which would read as a realm
      with no costs
  M7  no fabricated arithmetic: the block contains no derived expense or margin
  M8  L15 (witness r4 N4-1): the figures are query_character's (build_char_profile
      writes gold, merit and income_breakdown); query_realm writes none of
      them, and reading it there told every seat "monthly_income null, the bridge
      published no income_breakdown" (l14i geval-p eval_e_names). The doubles below
      serve the money on query_character and an emitter-shaped query_realm
      (query_handlers.cpp build_char_profile: r.key("gold"), r.key("merit"),
      r.key("income_breakdown") with total_monthly/gross_domain/net_domain/
      total_expenses/net_gold; do_query_realm: r.key("income") {monthly}).
"""
from __future__ import annotations

import asyncio
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import redesign as R                                             # noqa: E402

#: The breakdown shape as the bridge actually emits it, from a sealed d49 reply.
#: This is the 98.4% case: a real total, every component zero.
REAL_ALL_ZERO = {"total_monthly": 15.47, "gross_domain": 0, "net_domain": 0,
                 "total_expenses": 0, "net_gold": 0}


class _GW:
    def __init__(self, char):
        self._char = char

    async def read_many(self, specs):
        out = []
        for verb, _ in specs:
            if verb == "query_realm":
                out.append({"held_title_count": 1, "income": {"monthly": 99.0}})
            elif verb == "query_character":
                out.append({"age": 40, **self._char})
            else:
                out.append({})
        return out

    async def read(self, verb, params):
        return {}

    async def delta_records_since_last(self):
        return []

    def game(self):
        return {"speed": 2}

    @property
    def health(self):
        return {}


def _money(char):
    async def go():
        gw = _GW(char)
        oa, op = R._resolve_actor, R._dejure_pair

        async def actor(_g, cid, _t):
            return ({"id": cid or 1}, None)

        async def pair(_g, _c, keys, **kw):
            return [{"total": 1, "held": 1, "title_key": k} for k in keys]
        R._resolve_actor, R._dejure_pair = actor, pair
        try:
            env = await R.observe_impl(gw, character_id=1, depth="brief")
        finally:
            R._resolve_actor, R._dejure_pair = oa, op
        return ((env.get("result") or {}) if isinstance(env, dict) else {}).get("money")
    return asyncio.run(go())


class ObserveMoney(unittest.TestCase):

    def test_M1_gold_and_income_are_reported(self):
        m = _money({"gold": 87.75, "merit": 0.0,
                    "income_breakdown": dict(REAL_ALL_ZERO, total_monthly=4.2)})
        self.assertEqual(m["gold"], 87.75)
        self.assertEqual(m["monthly_income"], 4.2)
        self.assertIs(m["in_debt"], False)

    def test_M2_an_all_zero_breakdown_is_ABSENT_with_a_reason(self):
        m = _money({"gold": 10, "merit": 0.0,
                    "income_breakdown": dict(REAL_ALL_ZERO)})
        self.assertIsNone(m["breakdown"],
                          "the 98.4% case must not pass zeros through")
        self.assertIn("unpopulated", m["breakdown_absent"])
        self.assertIn("not a realm", m["breakdown_absent"])

    def test_M3_a_partial_breakdown_names_what_is_still_missing(self):
        m = _money({"gold": 10, "merit": 0.0,
                    "income_breakdown": dict(REAL_ALL_ZERO, gross_domain=20.5)})
        self.assertEqual(m["breakdown"], {"gross_domain": 20.5})
        self.assertEqual(sorted(m["breakdown_partial"]),
                         ["net_domain", "net_gold", "total_expenses"])

    def test_M4_monthly_income_is_total_monthly_never_the_merit(self):
        """RED on 55a66b0c: money.monthly_income read query_character's
        `monthly_income` (the merit stock, now emitted as `merit`), so on this
        emitter-shaped reply (merit 430, total_monthly 5.68) it read None."""
        m = _money({"gold": 10, "merit": 430.0,
                    "income_breakdown": dict(REAL_ALL_ZERO, total_monthly=5.68)})
        self.assertEqual(m["monthly_income"], 5.68)
        self.assertNotIn("totals_disagree", m)
        self.assertNotIn(430.0, [v for v in m.values() if isinstance(v, float)])

    def test_M5_no_breakdown_no_income(self):
        m = _money({"gold": 10, "merit": 12.0})
        self.assertIsNone(m["monthly_income"],
                          "no income_breakdown: unmeasured, never 0 and never the merit")

    def test_M6_expenses_is_None_with_a_reason_never_zero(self):
        m = _money({"gold": 10, "merit": 0.0})
        self.assertIsNone(m["expenses"])
        self.assertIsNot(m["expenses"], 0)
        self.assertIn("not a statement that spending is zero",
                      m["expenses_absent"],
                      "the hub required this sentence IN THE REPLY so a seat "
                      "cannot read the absence as zero spending")
        self.assertIn("unpopulated rather than measured", m["expenses_absent"])

    def test_M7_no_fabricated_arithmetic(self):
        m = _money({"gold": 87.75, "merit": 0.0,
                    "income_breakdown": dict(REAL_ALL_ZERO)})
        for forbidden in ("margin", "net_monthly", "surplus", "profit",
                          "expenses_estimated", "runway"):
            self.assertNotIn(forbidden, m,
                             f"{forbidden!r} would be a number nothing sourced")


class ObserveMoneyIsTheCharacters(unittest.TestCase):

    def test_M8_the_money_is_read_off_query_character(self):
        """RED on 627af763: monthly_income None and breakdown_absent "the bridge
        published no income_breakdown" with both on the query_character reply."""
        m = _money({"gold": 204.0, "merit": 0.0,
                    "income_breakdown": dict(REAL_ALL_ZERO, total_monthly=6.5,
                                             gross_domain=7.0)})
        self.assertEqual((m["gold"], m["monthly_income"]), (204.0, 6.5))
        self.assertEqual(m["breakdown"], {"gross_domain": 7.0})
        self.assertNotIn("breakdown_absent", m)

    def test_M8_CONTROL_a_reply_without_a_breakdown_still_says_so(self):
        m = _money({"gold": 1.0, "merit": 2.0})
        self.assertEqual(m["breakdown_absent"], "the bridge published no income_breakdown")


if __name__ == "__main__":
    unittest.main()
