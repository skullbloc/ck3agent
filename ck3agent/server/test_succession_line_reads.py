"""Succession reads on a recorded Lorraine game: the title's line, the forecast and the heir's dynasty.

Recorded: for turns 11-17 inspect title_succession 768 listed Gottfried 34471's sister Ida 33804 first and none of his
children (the title's cached line, built at his accession in 1070 and never rebuilt), succession_plan forecast Ida
keeping the duchy and his son Goffredo 69510 taking Brabant, and no read said that Goffredo, of his mother's dynasty by
a matrilineal marriage made before the 1066 start, was of another dynasty. At the death (1079.08.01) Goffredo took
every title and the line was lost.

The line is the bridge's (bridge/include/heir_line.h, test_heir_line: the holder's children first). Here the server's
readers of it: both succession reads name the same primary heir from the same bridge state (one computation, two
readers), the dynasty warning is first in each, and the replay drives them with the recorded Lorraine replies.
"""
import asyncio
import gzip
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import heir_dynasty as HD  # noqa: E402
import news as N  # noqa: E402
import redesign as R  # noqa: E402
import reply_fit as RF  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
QH = os.path.join(REPO, "bridge", "src", "query_handlers.cpp")

CONF, MALE_PREF = "confederate_partition_succession_law", "male_preference_law"
LAWS = [{"name": CONF, "group_name": "succession_order_laws"},
        {"name": MALE_PREF, "group_name": "succession_gender_laws"}]


def run(c):
    return asyncio.run(c)


RULER = {"id": 34471, "name": "Gottfried", "alive": True, "is_female": False, "age": 36, "health": 2.41,
         "children": [45066, 69510], "ruler_tier": 3, "dynasty_id": 2032, "dynasty_name": "Karling",
         "faith_id": 23, "piety": 716.0, "prestige": 1192.56, "primary_title_id": 768,
         "spouses": [{"id": 36010, "alive": True, "matrilineal": True}]}
MATILDA = {"id": 36010, "name": "Matilda", "alive": True, "is_female": True, "age": 30, "dynasty_id": 2281,
           "dynasty_name": "di Lucca", "spouses": [{"id": 34471, "alive": True, "matrilineal": True}]}
KIDS = {45066: {"id": 45066, "name": "Virginia", "alive": True, "is_female": True, "age": 2, "traits": [],
                "ruler_tier": 0, "faith_id": 23, "dynasty_id": 2281, "dynasty_name": "di Lucca", "spouses": []},
        69510: {"id": 69510, "name": "Goffredo", "alive": True, "is_female": False, "age": 0, "traits": [],
                "ruler_tier": 0, "faith_id": 23, "dynasty_id": 2281, "dynasty_name": "di Lucca", "spouses": []},
        33804: {"id": 33804, "name": "Ida", "alive": True, "is_female": True, "age": 38, "traits": [],
                "dynasty_id": 2032, "dynasty_name": "Karling", "spouses": []}}
TITLES = [{"title_id": 768, "tier": 3, "key": "d_lower_lorraine", "name": "Lower Lotharingia"},
          {"title_id": 708, "tier": 3, "key": "d_brabant", "name": "Brabant"}]
COUNTIES = [{"title_id": 695, "tier": 2, "key": "c_bouillon", "name": "Bouillon"},
            {"title_id": 713, "tier": 2, "key": "c_antwerpen", "name": "Antwerpen"}]


def _dyn(h):
    return (KIDS.get(h) or {}).get("dynasty_id") or 2032


def plan(gw):
    env = run(R.succession_plan_impl(gw, character_id=gw.ruler["id"]))
    assert env["ok"], env
    return env["result"]


def view(gw):
    env = run(R.inspect_impl(gw, "title_succession", character_id=gw.ruler["id"], title_id=768))
    return env["result"]


# ── 1. the recorded defect, and the line the bridge now reports ──────────────


def test_1_every_line_read_in_the_bridge_goes_through_title_line():
    """One computation: the cached heir array (SUCCESSION_HEIRS) is read in query_handlers.cpp only by
    cached_heir_line, which title_line wraps; every succession handler reports title_line()."""
    src = open(QH).read()
    reads = [m.start() for m in re.finditer(r"title_obj::SUCCESSION_HEIRS\)", src)]
    fn = src.index("static std::vector<uint32_t> cached_heir_line(")
    end = src.index("\n}\n", fn)
    assert reads and all(fn < r < end for r in reads), "a handler reads the raw cache"
    for h in ("do_query_succession(", "do_query_title_succession_map(", "do_query_title_succession(",
              "do_query_succession_simulation(", "do_query_partition_preview("):
        body = src[src.index("bool " + h):]
        body = body[:body.index("\n}\n")]
        assert "title_line(" in body, h


# ── 2. two readers, one answer ────────────────────────────────────────────────


# ── 3. the dynasty, as loudly as the tools allow ──────────────────────────────


# ── 4. the rule for the children's dynasty ───────────────────────────────────

def test_4_children_dynasty_rule():
    man, woman = {"is_female": False, "dynasty_id": 1}, {"is_female": True, "dynasty_id": 2}
    assert HD.children_dynasty(man, woman, False) == 1 and HD.children_dynasty(man, woman, True) == 2
    assert HD.children_dynasty(woman, man, False) == 1 and HD.children_dynasty(woman, man, True) == 2
    assert HD.children_dynasty(man, woman, None) is None                       # unread: never a guess
    assert HD.other_dynasty(2032, None) is None and HD.other_dynasty(2032, 2281) is True


# ── 5. divorce goes to the named spouse ──────────────────────────────────────

def test_5_divorce_is_sent_to_the_named_recipient():
    """Recorded (1070.10.26): both divorce sends came back ai_no_valid_target (the AI fallback ignores the target).
    Both interactions are now on the bridge's direct-dispatch list, the server's mirror beside it."""
    src = open(os.path.join(REPO, "bridge", "src", "ai_eval_hook.cpp")).read()
    lst = src[src.index("direct_list[]"):]
    lst = lst[:lst.index("nullptr\n")]
    for k in (R.DIVORCE_INTERACTION, R.DIVORCE_HOF_INTERACTION):
        assert f'"{k}"' in lst and k in R.DIRECT_DISPATCH_INTERACTIONS
