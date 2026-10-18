"""ck-m3xd — the money block reports what is real and says what is missing.

HOST ONLY. Pure shape over a reply; no bridge, no game.

MEASURED over d45-d50, 1,983 observations carrying `income_breakdown`:
  1,952 (98.4%)  EVERY component zero -- gross_domain, net_domain,
                 total_expenses, net_gold -- while total_monthly carried a real
                 number. A breakdown that breaks nothing down, and four zeros
                 that read as "no expenses" rather than "not populated".
     761 (38%)   income_breakdown.total_monthly DISAGREED with monthly_income.
                 Two fields for one quantity, differing, with nothing saying
                 which to believe.

THE LIMIT THIS BEAD DOES NOT EXCEED. The design asked for "income/expense lines
that sum". THERE IS NO EXPENSE SIDE: total_expenses is 0 in all 1,983. So the
reconciliation cannot be computed and is not faked -- `expenses` is None with a
reason. Deriving an expense line would be a fabricated number on a screen the
seat is meant to trust.

  M1  gold and monthly_income are reported
  M2  THE 98.4% CASE: an all-zero breakdown is reported ABSENT with a reason,
      never passed through as zeros
  M3  a partially populated breakdown reports the populated keys AND names the
      ones still missing
  M4  THE CONTROL AGAINST SILENT PREFERENCE: when the two totals disagree, BOTH
      are named and neither is chosen
  M5  and when they agree, there is no disagreement note (no crying wolf)
  M6  `expenses` is None with a reason -- never 0, which would read as a realm
      with no costs
  M7  no fabricated arithmetic: the block contains no derived expense or margin
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
    def __init__(self, realm):
        self._realm = realm

    async def read_many(self, specs):
        out = []
        for verb, _ in specs:
            if verb == "query_realm":
                out.append(self._realm)
            elif verb == "query_character":
                out.append({"character": {"age": 40}})
            else:
                out.append({})
        return out

    async def read(self, verb, params):
        return {}

    async def delta_since_last(self):
        return []

    def game(self):
        return {"speed": 2}

    @property
    def health(self):
        return {}


def _money(realm):
    async def go():
        gw = _GW(realm)
        oa, op = R._resolve_actor, R._settled_dejure_pair

        async def actor(_g, cid, _t):
            return ({"id": cid or 1}, None)

        async def pair(_g, _c, keys, **kw):
            return [{"total": 1, "held": 1, "title_key": k} for k in keys]
        R._resolve_actor, R._settled_dejure_pair = actor, pair
        try:
            env = await R.observe_impl(gw, character_id=1, depth="brief")
        finally:
            R._resolve_actor, R._settled_dejure_pair = oa, op
        return ((env.get("result") or {}) if isinstance(env, dict) else {}).get("money")
    return asyncio.run(go())


class ObserveMoney(unittest.TestCase):

    def test_M1_gold_and_income_are_reported(self):
        m = _money({"gold": 87.75, "monthly_income": 4.2})
        self.assertEqual(m["gold"], 87.75)
        self.assertEqual(m["monthly_income"], 4.2)
        self.assertIs(m["in_debt"], False)

    def test_M2_an_all_zero_breakdown_is_ABSENT_with_a_reason(self):
        m = _money({"gold": 10, "monthly_income": 15.47,
                    "income_breakdown": dict(REAL_ALL_ZERO)})
        self.assertIsNone(m["breakdown"],
                          "the 98.4% case must not pass zeros through")
        self.assertIn("unpopulated", m["breakdown_absent"])
        self.assertIn("not a realm", m["breakdown_absent"])

    def test_M3_a_partial_breakdown_names_what_is_still_missing(self):
        m = _money({"gold": 10, "monthly_income": 15.47,
                    "income_breakdown": dict(REAL_ALL_ZERO, gross_domain=20.5)})
        self.assertEqual(m["breakdown"], {"gross_domain": 20.5})
        self.assertEqual(sorted(m["breakdown_partial"]),
                         ["net_domain", "net_gold", "total_expenses"])

    def test_M4_CONTROL_disagreeing_totals_are_BOTH_named(self):
        m = _money({"gold": 10, "monthly_income": 4.2,
                    "income_breakdown": dict(REAL_ALL_ZERO)})  # total_monthly 15.47
        d = m.get("totals_disagree")
        self.assertIsNotNone(d, "38% of real observations are this case")
        self.assertEqual(d["monthly_income"], 4.2)
        self.assertEqual(d["income_breakdown.total_monthly"], 15.47)
        self.assertIn("neither is preferred", d["note"])

    def test_M5_agreeing_totals_raise_no_note(self):
        m = _money({"gold": 10, "monthly_income": 15.47,
                    "income_breakdown": dict(REAL_ALL_ZERO)})
        self.assertNotIn("totals_disagree", m,
                         "a note on every call is noise")

    def test_M6_expenses_is_None_with_a_reason_never_zero(self):
        m = _money({"gold": 10, "monthly_income": 4.2})
        self.assertIsNone(m["expenses"])
        self.assertIsNot(m["expenses"], 0)
        self.assertIn("not a statement that spending is zero",
                      m["expenses_absent"],
                      "the hub required this sentence IN THE REPLY so a seat "
                      "cannot read the absence as zero spending")
        self.assertIn("unpopulated rather than measured", m["expenses_absent"])

    def test_M7_no_fabricated_arithmetic(self):
        m = _money({"gold": 87.75, "monthly_income": 4.2,
                    "income_breakdown": dict(REAL_ALL_ZERO)})
        for forbidden in ("margin", "net_monthly", "surplus", "profit",
                          "expenses_estimated", "runway"):
            self.assertNotIn(forbidden, m,
                             f"{forbidden!r} would be a number nothing sourced")


if __name__ == "__main__":
    unittest.main()
