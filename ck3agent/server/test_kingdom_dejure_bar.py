"""Small follow-ups to the seat toolset.

The kingdom de jure bar is the game's rule (at least half), not the bridge's old 51%; the server's
readings and the bridge's gate (bridge/include/dejure_bar.h, built and ctest-run, not proven live) read one table.
"""
import re
from pathlib import Path

import pytest

import redesign as R

ROOT = Path(__file__).resolve().parents[2]
BAR_H = ROOT / "bridge" / "include" / "dejure_bar.h"
BAR_TEST = ROOT / "bridge" / "test" / "test_dejure_bar.cpp"


# ── The kingdom de jure bar ───────────────────────────────────────────────────────────────────

def test_2_a_kingdom_needs_half_its_counties_not_51_percent():
    # More than half (the game's own "required Counties" lines), not the 51% bar and not half
    assert R.dejure_need(46, 4) == 24 and R.dejure_need(100, 4) == 51 and R.dejure_need(47, 4) == 24
    assert R.mint_dejure_shortfall("k_x", 24, 46) is None
    assert R.mint_dejure_shortfall("k_x", 23, 46) == {"held": 23, "need": 24, "total": 46}
    # the empire row is unchanged: more than 80%
    assert R.dejure_need(117, 5) == 94 and R.dejure_need(100, 5) == 81
    # a tier with no game text (a hegemony), or one the key does not name, keeps the bridge's 51%
    assert R.dejure_need(100, 6) == 51 and R.dejure_need(100) == 51


def test_2_the_rule_text_says_the_games_rule_and_no_51_percent_for_a_kingdom():
    t = R.MINT_DEJURE_RULE
    assert "more than half" in t and "more than 80%" in t and "51%" not in t, t


def test_2_the_server_mirrors_the_bridge_header():
    src = BAR_H.read_text(encoding="utf-8")

    def c(name):
        return int(re.search(rf"\b{name}\s*=\s*(\d+)", src).group(1))
    assert c("CREATE_TITLE_DEJURE_MIN_TIER") == R.MINT_DEJURE_MIN_TIER
    assert R.GAME_DEJURE_SHARE[4] == (True, c("CREATE_TITLE_DEJURE_HALF_NUM"), c("CREATE_TITLE_DEJURE_HALF_DEN"))
    assert "held * CREATE_TITLE_DEJURE_HALF_DEN <= total * CREATE_TITLE_DEJURE_HALF_NUM" in src   # More than
    assert R.GAME_DEJURE_SHARE[c("CREATE_TITLE_DEJURE_EMPIRE_TIER")] == (
        True, c("CREATE_TITLE_DEJURE_EMPIRE_NUM"), c("CREATE_TITLE_DEJURE_EMPIRE_DEN"))
    assert c("CREATE_TITLE_DEJURE_OTHER_PCT") == round(R.MINT_DEJURE_MIN_FRAC * 100)
    gate = (ROOT / "bridge" / "src" / "intent_dispatch.cpp").read_text(encoding="utf-8")
    assert "bridge::dejure_bar::short_of(tgt_tier, held, total)" in gate
    assert "CREATE_TITLE_DEJURE_MIN_FRAC" not in gate


def test_2_the_bridge_test_rows_agree_with_the_server():
    """The C++ test's rows (bridge/test/test_dejure_bar.cpp) graded by the server's dejure_need: one table, two
    readers. Rows outside the gate (a duchy, a titular title) are the caller's, not dejure_need's."""
    rows = re.findall(r'\{"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(true|false),\s*(true|false)\}',
                      BAR_TEST.read_text(encoding="utf-8"))
    assert len(rows) >= 12, rows
    graded = 0
    for what, tier, held, total, short, _ in rows:
        tier, held, total = int(tier), int(held), int(total)
        if tier < R.MINT_DEJURE_MIN_TIER or total == 0:
            continue
        assert (held < R.dejure_need(total, tier)) == (short == "true"), what
        graded += 1
    assert graded >= 10


# ── minor_religious_war's title rule and outcome rows, from the game's files ──────────────────

import cb_labels as CBL  # noqa: E402
import declare_gates as DG  # noqa: E402

GAME = Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game"
RELIGIOUS = GAME / "common" / "casus_belli_types" / "00_religious_war.txt"


def test_3a_minor_religious_war_has_a_title_rule_and_an_outcome_row():
    scope, grants, requires, source = CBL.LABELS["minor_religious_war"]
    assert scope == "county" and "00_religious_war.txt" in source
    assert any("hostile" in r for r in requires) and any("100 piety" in r for r in requires)
    o = CBL.relabel({"cb": "minor_religious_war", "scope": "faith_counties", "grants": "x"})
    assert o["scope"] == "county" and o["bridge_label"]["scope"] == "faith_counties"
    row = R.PEACE_PRICE["minor_religious_war"]
    assert row["gold_years"] == 2 and row["file"] == "00_religious_war.txt"
    p = R.peace_prices("minor_religious_war", "attacker", 10.0, ["we_win", "white_peace", "we_yield"])
    assert p["we_yield"]["gold"] == -240.0 and "x2" not in p["we_yield"]["gold_basis"], p["we_yield"]
    assert "50 piety" in p["white_peace"]["you"] and p["white_peace"]["gold"] == 0
    assert p["we_yield"]["prestige"] == 0 and p["white_peace"]["prestige"] == 0
    assert "unknown" not in str(R.loss_price("minor_religious_war", 10.0))


def test_3a_CONTROL_a_casus_belli_without_rows_still_reads_unknown():
    p = R.peace_prices("major_religious_war", "attacker", 10.0, ["we_yield"])
    assert p["we_yield"]["price"] == "unknown"


def test_3a_the_bridge_table_carries_the_same_words():
    src = (ROOT / "bridge" / "src" / "query_handlers.cpp").read_text(encoding="utf-8")
    scope, grants, _r, _s = CBL.LABELS["minor_religious_war"]
    m = re.search(r'\{"minor_religious_war",\s*"([^"]*)",\s*"([^"]*)"\}', src)
    assert m and (m.group(1), m.group(2)) == (scope, grants)


@pytest.mark.skipif(not RELIGIOUS.is_file(), reason="needs the game's files")
def test_3a_the_rows_are_the_files():
    t = RELIGIOUS.read_text(encoding="utf-8-sig")
    body = t[t.index("minor_religious_war = {"):t.index("\nreligious_war = {")]
    assert re.search(r"valid_to_start = \{\s*scope:target = \{\s*tier = tier_county", body)
    assert "target_titles = neighbor_land_or_water" in body
    assert "value >= religious_cb_enabled_hostility_level" in body
    assert re.search(r"piety = \{\s*add = \{\s*value = 100", body)
    defeat = body[body.index("on_defeat = {"):body.index("on_primary_attacker_death")]
    assert re.search(r"pay_short_term_income = \{\s*years = 2", defeat)
    assert "add_prestige" not in body                    # no prestige on any ending
    white = body[body.index("on_white_peace = {"):body.index("on_defeat_desc")]
    assert "add_piety = religious_cb_piety_white_peace" in white
    vals = (GAME / "common" / "script_values" / "02_religion_values.txt").read_text(encoding="utf-8-sig")
    assert re.search(r"^religious_cb_piety_white_peace = -50\b", vals, re.M)
    assert re.search(r"^religious_cb_piety_gain_county = 50\b", vals, re.M)


# ── the map digest's adjacency on a large region ─────────────────────────────────────────────

import asyncio  # noqa: E402

import map_view as MV  # noqa: E402
from test_ireland_walk_replies import Gw  # noqa: E402

ME35 = 12015


def _chain(n, prefix="c_synthetic_county_"):
    """n counties in a line, each bordering the one before and after; and 2 border counties at the ends."""
    keys = [f"{prefix}{i:03d}" for i in range(n)]
    edges = {k: set() for k in keys + ["c_out_a", "c_out_b"]}
    for a, b in zip(["c_out_a"] + keys, keys + ["c_out_b"]):
        edges[a].add(b)
        edges[b].add(a)
    return keys, edges


def _bridge_graph(edges, baked=None):
    """query_map_graph as map_graph.cpp answers it, with json_get_string's 256-byte buffer: the counties string
    cut at 255 bytes; seeds plus one hop; edges of the induced subgraph."""
    baked = set(edges) if baked is None else baked

    def reply(p):
        raw = str(p.get("counties") or "")[:255]
        seeds = [k for k in (t.strip() for t in raw.split(",")) if k in baked]
        order = list(dict.fromkeys(seeds))
        for s in seeds:
            for nb in sorted(edges[s]):
                if nb in baked and nb not in order:
                    order.append(nb)
        win = set(order)
        lines = ["# CK3MAP scope=explicit hops=1 dist=px", "# legend: ...", "# NODES"]
        lines += [f"@{k} +d_x/k_x/e_x" for k in order]
        lines.append("# EDGES")
        for k in order:
            nb = [x for x in sorted(edges[k]) if x in win]
            if nb:
                lines.append(f"@{k} > " + " ".join(f"{x}:50" for x in nb))
        return {"text": "\n".join(lines) + "\n", "status": "ok", "truncated": False}
    return reply


def _map_env(monkeypatch, keys, edges, baked=None):
    import title_table as TT

    async def tt(gw, **kw):
        rows = [{"key": k, "title_id": 1000 + i, "holder_id": ME35, "realm_ruler_id": ME35, "you_control": True}
                for i, k in enumerate(keys)]
        return {"title_key": "e_x", "counties": rows, "duchies": [], "realms": [],
                "summary": {"held": len(keys), "total": len(keys), "creation_needs_counties": len(keys)}}, [], None
    monkeypatch.setattr(TT, "title_table", tt)
    gw = Gw({"query_map_graph": _bridge_graph(edges, baked), "query_realm": {"military_strength": 100},
             "query_create_title_cost": {}}, date="0867.01.01")
    env = asyncio.run(R.inspect_impl(gw, subject="map", character_id=ME35, title_key="e_x",
                                     layers=["holders", "adj"]))
    return env, gw


def test_3b_every_county_of_a_long_region_has_its_neighbours(monkeypatch):
    keys, edges = _chain(40)
    env, gw = _map_env(monkeypatch, keys, edges)
    res = env["result"]
    rows = {ln.split()[0]: ln for ln in res["text"].splitlines() if not ln.startswith("#")}
    for k in keys:
        want = {MV.handle(x) for x in edges[k]}
        got = set(re.search(r"adj=(\S+)", rows[MV.handle(k)]).group(1).split(","))
        assert got == want, (k, rows[MV.handle(k)])
    sent = [p["counties"] for a, p in gw.reads if a == "query_map_graph"]
    assert len(sent) > 1 and all(len(s.encode()) <= MV.MAP_ARG_MAX for s in sent)
    assert sorted(",".join(sent).split(",")) == sorted(keys)
    assert "adj" not in (res.get("unread") or {}) and "truncated" not in res


def test_3b_a_county_the_baked_graph_lacks_is_named(monkeypatch):
    keys, edges = _chain(6)
    env, _ = _map_env(monkeypatch, keys, edges, baked=set(edges) - {keys[2]})
    res = env["result"]
    assert MV.handle(keys[2]) + " YOU adj=?" in res["text"]
    assert res["unread"]["adj"].startswith("adj=? on 1: " + MV.handle(keys[2]))


def test_3b_a_region_over_the_size_cuts_border_then_adjacency_and_says_so(monkeypatch):
    """e_france's size: 117 counties of about ten-letter handles (each line holds its two neighbours here; a
    French county has four to eight, so the live reply cuts more rows)."""
    n = 200
    keys, edges = _chain(n, prefix="c_county_")
    env, _ = _map_env(monkeypatch, keys, edges)
    res = env["result"]
    import reply_fit as RF
    assert RF.size({k: v for k, v in res.items() if k != "subject"}) <= MV.MAP_RESULT_MAX
    assert RF.size(env) <= RF.CAP, RF.size(env)                       # the whole reply, envelope and all
    tr = res["truncated"]
    assert tr["border"] == {"shown": 0, "of": 2} and tr["adj"]["of"] == n and 0 < tr["adj"]["shown"] < n
    cut = [ln for ln in res["text"].splitlines() if ln.endswith("adj=cut")]
    assert len(cut) == n - tr["adj"]["shown"] and "adj=?" not in res["text"]
    assert all(MV.handle(k) in res["text"] for k in keys)            # a region row is never cut


def test_3b_CONTROL_a_region_under_the_size_is_whole(monkeypatch):
    keys, edges = _chain(60, prefix="c_county_")
    res = _map_env(monkeypatch, keys, edges)[0]["result"]
    assert "truncated" not in res and "adj=cut" not in res["text"] and sum(ln.endswith(" (out)") for ln in res["text"].splitlines()) == 2


def test_3b_CONTROL_a_short_region_is_one_read():
    assert MV.county_chunks(["c_a", "c_b"]) == ["c_a,c_b"]
    assert MV.county_chunks([]) == []
    ks = [f"c_{i:010d}" for i in range(40)]
    ch = MV.county_chunks(ks)
    assert ",".join(ch) == ",".join(ks) and max(len(c) for c in ch) <= 255


# ── query vassal_loyalty answers every vassal within 8 KB ─────────────────────────────────────

import json  # noqa: E402

FIXTURE_VL = Path(__file__).resolve().parent / "fixtures" / "vassal_loyalty_867.json"


def _vassals_30():
    """A recorded game, 867 seq 62 (Charles II, West Francia; fixtures/vassal_loyalty_867.json names the run):
    the 21 rows the reply showed of 30, and 9 more of the same shapes (the cut rows were not sent)."""
    res = json.loads(FIXTURE_VL.read_text(encoding="utf-8"))["result"]
    assert res["vassal_count"] == 30 and res["truncated"]["shown"] == 21       # the hazard, as recorded
    rows = [dict(r) for r in res["vassals"]]
    rows += [dict(rows[i % 21], id=90000 + i, name=f"Extra{i}") for i in range(9)]
    keep = {k: res[k] for k in ("action", "ruler_id", "vassal_count", "count_disloyal", "count_in_faction")}
    return dict(keep, vassals=rows)


def test_3c_query_vassal_loyalty_answers_all_30_vassals_within_8_kb(monkeypatch):
    import mcp_server as ms
    from test_query_tool import Bridge
    reply = _vassals_30()
    state = ms.BridgeState()
    state.connected = True
    b = Bridge(state)
    b.replies["query_vassal_loyalty"] = reply
    monkeypatch.setattr(ms, "_get_state", lambda: state)

    async def qb(_s, action, params, timeout=15.0):
        return b.answer("query", action, params, timeout)

    async def bound(*_a):
        return None
    monkeypatch.setattr(ms, "_query_bridge", qb)
    monkeypatch.setattr(R, "unbound_refusal", bound)
    env = asyncio.run(ms.query(domain="vassal_loyalty"))
    res = env["result"]
    import reply_fit as RF
    assert len(res["vassals"]) == 30 == res["vassal_count"] and "truncated" not in res, res.get("truncated")
    assert RF.size(env) <= RF.CAP
    by_id = {r["id"]: r for r in res["vassals"]}
    r0 = reply["vassals"][0]
    assert by_id[r0["id"]]["contract"] == {k: v for k, v in r0["contract"].items() if v}
    assert "level 0" in res["contract_levels"]


def test_3c_CONTROL_fit_reply_alone_still_cuts_the_full_rows():
    import mcp_server as ms
    out = ms.fit_reply(_vassals_30(), ms.QUERY_REPLY_CAP)
    assert out["truncated"]["of"] == 30 and out["truncated"]["shown"] < 30


# ── The men-at-arms price is the base before the ruler's modifiers, and says so ────────────────

MAA = GAME / "common" / "men_at_arms_types" / "00_maa_types.txt"


def _maa_env(currency):
    from test_ireland_walk_replies import Gw as _Gw
    t = {"maa_type": "light_footmen", "recruit_cost": 45.0, "upkeep_per_stack": 0.15, "fielded_regiments": 0,
         "affordable": True}
    if currency == "prestige":
        t.update(price=90.0, currency="prestige")
    mt = {"actor_id": ME35, "gold": 91.0, "prestige": 600.0, "types": [t],
          **({"currency": "prestige", "government": "tribal_government", "price_basis": "x2"}
             if currency == "prestige" else {"currency": "gold"})}
    return asyncio.run(R.do_impl(_Gw({"query_maa_types": mt}), action_class="men_at_arms", character_id=ME35))


def test_4_every_listed_price_says_before_modifiers():
    for cur, price in (("prestige", 90.0), ("gold", 45.0)):
        env = _maa_env(cur)
        rows = env["result"].get("available") or env["result"].get("unavailable")
        assert rows[0]["label"] == f"light_footmen (recruit {price} {cur} before modifiers)", rows[0]["label"]
    assert "men_at_arms_recruitment_cost" in R.MAA_PRICE_BEFORE_MODIFIERS
    assert "bellicose" in R.MAA_PRICE_BEFORE_MODIFIERS


@pytest.mark.skipif(not MAA.is_file(), reason="needs the game's files")
def test_4_the_81_of_90_is_a_bellicose_culture():
    """a live check (1066, Hummay 36774, culture kanuri): light_footmen listed 90 prestige, charged 81."""
    c = (GAME / "common/culture/cultures/00_central_african.txt").read_text(encoding="utf-8-sig")
    kan = c[c.index("kanuri = {"):]
    assert re.search(r"ethos = ethos_bellicose", kan[:kan.index("\n}")])
    e = (GAME / "common/culture/pillars/00_ethos.txt").read_text(encoding="utf-8-sig")
    bel = e[e.index("ethos_bellicose = {"):e.index("\n}", e.index("ethos_bellicose = {"))]
    m = float(re.search(r"men_at_arms_recruitment_cost = (-?[0-9.]+)", bel).group(1))
    t = MAA.read_text(encoding="utf-8-sig")
    lf = t[t.index("light_footmen = {"):]
    assert "buy_cost = { gold = skirmisher_recruitment_cost }" in lf[:lf.index("\n}")]
    sv = (GAME / "common/script_values/00_men_at_arms_values.txt").read_text(encoding="utf-8-sig")
    base = float(re.search(r"^@skirmisher_recruitment_cost = ([0-9.]+)", sv, re.M).group(1))
    assert base * 2 == 90.0 and round(base * 2 * (1 + m), 2) == 81.0


# ── The listing with the army switches on ─────────────────────────────────────────────────────

import os  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402

import army_orders as AO  # noqa: E402

#: The default listing's caps (test_map_digest_and_turn_one_notes, test_decision_veto): the listing a counted seat receives, both switches off.
DEFAULT_CAPS = (31_049, 36_298)
#: The cap for a listing with an army switch on. The default caps are measured ceilings of the
#: listing every counted seat is sent (no arm turns a switch on); a switched build adds do's army sentence and
#: its args row (~250 B), which do not fit under them without cutting the default text. So the switched listings
#: get their own ceiling, the largest measured (both on: 31,350 / 36,528) plus 50 B, and each is checked here.
SWITCHED_CAPS = (31_400, 36_578)

_LISTING = r"""
import asyncio, json, sys
sys.path.insert(0, sys.argv[1])
import mcp_server as ms
out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in asyncio.run(ms.mcp.list_tools())]
out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]} for d in out]
args = next(o for o in out if o["name"] == "do")["inputSchema"]["properties"]["args"]["description"]
row = next((ln.strip() for ln in args.splitlines() if ln.strip().startswith("army:")), None)
print(json.dumps({"n": len(out), "raw": len(json.dumps(out)), "indent": len(json.dumps(out, indent=1)), "row": row}))
"""


def _listing(**env):
    e = {k: v for k, v in os.environ.items() if k not in (AO.ENV_ORDERS, AO.ENV_HOLD)}
    e.update(env)
    r = subprocess.run([sys.executable, "-c", _LISTING, str(Path(__file__).resolve().parent)], env=e,
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stderr[-2000:]
    return json.loads(r.stdout.strip().splitlines()[-1])


def test_5_the_switched_listings_mark_raid_and_fit_their_cap():
    sizes = {}
    for name, env in (("default", {}), ("orders", {AO.ENV_ORDERS: "1"}), ("hold", {AO.ENV_HOLD: "1"}),
                      ("both", {AO.ENV_ORDERS: "1", AO.ENV_HOLD: "1"})):
        got = _listing(**env)
        assert got["n"] == 13, (name, got)
        sizes[name] = (got["raw"], got["indent"])
        if name == "default":
            assert got["row"] is None and got["raw"] <= DEFAULT_CAPS[0] and got["indent"] <= DEFAULT_CAPS[1]
            continue
        assert got["raw"] <= SWITCHED_CAPS[0] and got["indent"] <= SWITCHED_CAPS[1], (name, got)
        acts = re.search(r"action=([^,]+),", got["row"]).group(1).split("|")
        if name in ("orders", "both"):
            # Raid is proven live (a recorded game rows 9-10) and listed plain
            assert "raid" in acts and not any("unproven" in a for a in acts), got["row"]
        else:
            assert not any(a.startswith("raid") for a in acts), got["row"]
        assert ("hold" in acts) == (name != "orders"), got["row"]
    # the env-on sizes as measured at the change's tip (31,266 B with CK3_BRIDGE_ARMY_ORDERS=1)
    assert sizes["orders"][0] > DEFAULT_CAPS[0] and sizes["orders"][0] <= SWITCHED_CAPS[0], sizes


def test_5_CONTROL_the_dispatch_still_takes_raid():
    assert "raid" in AO.ARMY_ACTIONS and AO.listed_actions({AO.ENV_ORDERS: "1"}) == ("disband", "move", "raid")
    assert AO.listed_actions({}) == ()
