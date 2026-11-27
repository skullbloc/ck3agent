"""inspect(subject="map"): the layered map digest (from the map study's prototype).

One fixed-order text line per county of a region: the de jure counties of a kingdom (by default the
kingdom of the seat's primary title, the target or next kingdom of a count or duke), in the game's de
jure order, then the counties bordering it. Columns by `layers`; the legend once; each row starts with
a short handle (the county key without `c_`, which every title argument takes as `c_<handle>`), and
the reply's `calls` give the ready deep-dive call for a row. A value no read gives prints `?` and the
reason is said once in `unread`, never guessed. On recorded questions the digest answered 12.5 of 13
against 6 for the raw replies, on an eighth of the tokens.

Composed from reads the bridge already serves:
  title_table   the region's holders, realm rulers, strengths, claims (query_dejure_progress, query_de_jure,
                query_claims, query_realm: the inspect title read, without its per-row calls)
  query_map_graph  scope=explicit, the region's counties in pieces of 255 bytes (MAP_ARG_MAX), one hop: adjacency (the baked ck3map.bin) and the
                live overlay (holder, development, control, faith, culture) of the region and its border
  declarable_now   the casus belli the cbs read gives against each realm ruler (cb layer)
  query_wars / query_armies   your wars' goals and sieges (with the besieger) and your armies
  query_de_jure   the barony of a siege's holding id and of an army's location, and its county
  query_armies others_in_counties  other rulers' armies in the region's counties (not the border's)
Not read by any one read (the layer says `?` for them): sieges in wars you are not in; which county is occupied
by whom.
"""
from __future__ import annotations

import re
from typing import Any, Optional

#: layer -> the legend's words (one line, in this order)
LAYERS = ("holders", "cb", "mil", "str", "adj", "duchy", "land")
DEFAULT_LAYERS = ("holders", "cb", "mil", "str", "adj")
LEGEND = {
    "holders": "holder id:name[/realm ruler] (YOU=your realm)",
    "cb": "cb=declarable now (!=a check refuses it now: cbs says why; -=none)",
    "mil": "WAR=your war's goal; army|other id:men@barony by owner; siege barony:%:status:besieger",
    "str": "realms: strength xyours alN=with allies likely to join",
    "adj": "adj (~sea ^river x impassable)",
    "duchy": "d=duchy (*yours)",
    "land": "dev/control/faith/culture/terrain",
}
#: the most realm rulers (in the kingdom and on its border) the cb layer reads casus belli for
MAX_CB_REALMS = 12
#: the most barony lookups (siege holdings, army locations) one digest makes
MAX_BARONY_READS = 16
#: the most border holders read for their realm ruler and strength
MAX_BORDER_HOLDERS = 12
#: The most county ids one query_armies others_in_counties read takes (bridge armies_around.h kCountiesMax)
OTHERS_IDS_MAX = 64
_SIGIL = {"s": "~", "r": "^", "x": "x"}
#: The bridge copies a string argument into a 256-byte buffer (json_helpers.cpp json_get_string), so a `counties` list
#: longer than 255 bytes was cut there and the counties past the cut were never seeds. The digest sends the region
#: in pieces of at most this many bytes and joins the graphs.
MAP_ARG_MAX = 255
#: the digest's result is kept at most this many bytes (compact JSON) so the reply and its envelope fit the read
#: tools' 8 KB (reply_fit.CAP); beyond it the border lines go first, then the adjacency of the last rows, each
#: counted in `truncated`
MAP_RESULT_MAX = 7000
_CB_SHORT = {"make_tributary_cb": "trib"}

#: baselines for observe's map news: (baseline, actor id) -> {"region", "date", "holders", "sieges"}
_LAST: dict = {}
#: actor id -> the date of the seat's last map look
_LOOKED: dict = {}
#: county title id -> key, from the digests and title reads this server made
_KEYS: dict = {}


def reset() -> None:
    _LAST.clear()
    _LOOKED.clear()
    _KEYS.clear()


def _d(x) -> dict:
    return x if isinstance(x, dict) else {}


def _i(v) -> Optional[int]:
    return v if isinstance(v, int) and not isinstance(v, bool) else None


def handle(key: Any) -> str:
    k = str(key or "?")
    return k[2:] if len(k) > 2 and k[1] == "_" else k


def parse_layers(layers: Any) -> tuple[Optional[tuple], Optional[str]]:
    """(layers, error). A list or a comma string; empty = the defaults."""
    if layers in (None, "", []):
        return DEFAULT_LAYERS, None
    items = layers if isinstance(layers, (list, tuple)) else str(layers).split(",")
    out = [str(x).strip() for x in items if str(x).strip()]
    bad = [x for x in out if x not in LAYERS]
    if bad:
        return None, f"unknown layers {bad}; layers: {', '.join(LAYERS)}"
    return tuple(x for x in LAYERS if x in out), None


def parse_graph(text: str) -> dict:
    """query_map_graph's text -> {county key: {duchy, kingdom, terrain, holder, faith, culture, dev, ctl,
    edges: {key: kind}}}; kind is land|sea|river|impassable."""
    nodes: dict = {}
    section = None
    kinds = {"s": "sea", "r": "river", "x": "impassable"}
    for line in (text or "").splitlines():
        if line.startswith("# NODES"):
            section = "n"
            continue
        if line.startswith("# EDGES"):
            section = "e"
            continue
        if line.startswith("#") or not line.startswith("@"):
            continue
        if section == "n":
            toks = line.split()
            key = toks[0][1:]
            n = nodes.setdefault(key, {"edges": {}})
            for t in toks[1:]:
                if t.startswith("+"):
                    parts = t[1:].split("/")
                    n["duchy"] = parts[0] if parts and parts[0] != "-" else None
                    n["kingdom"] = parts[1] if len(parts) > 1 and parts[1] != "-" else None
                elif t.startswith("T"):
                    n["terrain"] = t[1:]
                elif t.startswith("!") and t[1:].isdigit():
                    n["holder"] = int(t[1:])
                elif t.startswith("~"):
                    n["faith"] = t[1:]
                elif t.startswith("*"):
                    n["culture"] = t[1:]
                elif re.fullmatch(r"dev\d+", t):
                    n["dev"] = int(t[3:])
                elif re.fullmatch(r"ctl\d+", t):
                    n["ctl"] = int(t[3:])
        elif section == "e" and ">" in line:
            head, _, rest = line.partition(">")
            key = head.strip()[1:]
            n = nodes.setdefault(key, {"edges": {}})
            for t in rest.split():
                p = t.split(":")
                n["edges"][p[0]] = kinds.get(p[2], "land") if len(p) > 2 else "land"
    return nodes


def county_chunks(keys: list, limit: int = MAP_ARG_MAX) -> list:
    """The region's county keys as comma lists of at most `limit` bytes each (a key longer than that alone)."""
    out, cur = [], ""
    for k in keys:
        nxt = f"{cur},{k}" if cur else k
        if cur and len(nxt.encode("utf-8")) > limit:
            out.append(cur)
            nxt = k
        cur = nxt
    return out + ([cur] if cur else [])


def merge_graphs(graphs: list) -> dict:
    """parse_graph results joined: a county's fields from the first read that has it, its edges from all (each
    read's edges are true edges; a seed's are all of its edges)."""
    out: dict = {}
    for g in graphs:
        for k, n in (g or {}).items():
            m = out.setdefault(k, {"edges": {}})
            for f, v in n.items():
                if f == "edges":
                    m["edges"].update(v)
                else:
                    m.setdefault(f, v)
    return out


async def default_region(gw, me: int) -> tuple[Optional[str], str]:
    """(the kingdom of the ruler's primary title, how it was chosen) or (None, why)."""
    rep = _d(await gw.read("query_de_jure", {"character_id": me}))
    title = _d(rep.get("title"))
    if not title:
        return None, "query_de_jure of your primary title did not answer"
    if _i(title.get("tier")) == 4:
        return title.get("key"), f"your primary title {title.get('key')}"
    for p in rep.get("de_jure_parents") or []:
        if isinstance(p, dict) and _i(p.get("tier")) == 4 and p.get("key"):
            return p["key"], f"the de jure kingdom of your primary title {title.get('key')}"
    return None, f"your primary title {title.get('key')} has no de jure kingdom above it: name one with title_key"


async def _barony_counties(gw, by_id: list, by_key: list) -> dict:
    """{barony id or key: (barony key, county key)} through query_de_jure, at most MAX_BARONY_READS."""
    reqs = [("query_de_jure", {"title_id": t}) for t in by_id] + [("query_de_jure", {"title_key": k}) for k in by_key]
    reqs = reqs[:MAX_BARONY_READS]
    got = await gw.read_many(reqs) if reqs else []
    out: dict = {}
    for (_, p), rep in zip(reqs, got):
        t = _d(_d(rep).get("title"))
        if not t:
            continue
        out[p.get("title_id") or p.get("title_key")] = (t.get("key"), t.get("de_jure_parent_key"))
    return out


async def name_sieges(gw, sieges: list) -> list:
    """Each siege row with `holding` {barony, county} beside its holding_title_id (the study's read defect:
    a siege named its holding by runtime id only). Rows whose barony was not read keep the id alone."""
    rows = [s for s in sieges or [] if isinstance(s, dict) and _i(s.get("holding_title_id"))]
    ids = sorted({s["holding_title_id"] for s in rows})
    names = await _barony_counties(gw, ids, []) if ids else {}
    for s in rows:
        b = names.get(s["holding_title_id"])
        if b:
            s["holding"] = {"barony": b[0], "county": b[1]}
    return sieges


async def digest(gw, *, me: int, title_key: str = "", layers: Any = None) -> tuple[dict, list, Optional[dict]]:
    """The inspect(subject='map') result: (result, errors, blocked)."""
    import redesign as R
    import title_table as _tt
    lay, bad = parse_layers(layers)
    if bad:
        return {}, [], R.why_blocked("invalid_argument", summary=bad)
    why_region = f"title_key={title_key}"
    if not title_key:
        title_key, why_region = await default_region(gw, me)
        if not title_key:
            return {}, [], R.why_blocked("missing_required_argument",
                                         summary=f"no default region ({why_region}); pass title_key='k_...'")
    res, errors, blk = await _tt.title_table(gw, character_id=me, title_key=title_key, next_calls=False)
    if blk is not None:
        return {}, errors, blk
    rows = [r for r in res.get("counties") or [] if isinstance(r, dict)]
    keys = [r.get("key") for r in rows if r.get("key")]
    _KEYS.update({r["title_id"]: r["key"] for r in rows if _i(r.get("title_id")) and r.get("key")})
    unread: dict = {}
    want_mil = "mil" in lay
    chunks = county_chunks(keys) or [""]
    reads = [("query_map_graph", {"character_id": me, "scope": "explicit", "counties": c,
                                  "hops": 1, "max_nodes": 200}) for c in chunks]
    reads += [("query_character", {"character_id": me}),
              ("query_realm", {"character_id": me}),
              ("query_create_title_cost", {"actor_id": me, "title_key": res.get("title_key") or title_key})]
    if want_mil:
        reads += [("query_wars", {"character_id": me}), ("query_armies", {"character_id": me})]
    got = await gw.read_many(reads)
    graph_raws = [_d(x) for x in got[:len(chunks)]]
    me_chr, me_realm, cost = (_d(x) for x in got[len(chunks):len(chunks) + 3])
    wars_raw, armies_raw = ((_d(got[len(chunks) + 3]), _d(got[len(chunks) + 4])) if want_mil else ({}, {}))
    graph = merge_graphs([parse_graph(g.get("text")) for g in graph_raws if g.get("text")])
    graph_raw = next((g for g in graph_raws if not g.get("text")), {})
    if not graph:
        unread["map"] = ("the bridge's map graph did not answer"
                         + (f" ({graph_raw.get('error')}: {graph_raw.get('hint', '')})" if graph_raw.get("error") else "")
                         + ": adjacency, border counties and dev/control are unread")
    else:
        # A county with no adjacency is named, never a silent `?`
        lost = sum(1 for g in graph_raws if not g.get("text"))
        no_adj = [k for k in keys if not (graph.get(k) or {}).get("edges")]
        if lost or no_adj:
            unread["adj"] = (f"adj=? on {len(no_adj)}: " + ",".join(handle(k) for k in no_adj[:24])
                             + (f" and {len(no_adj) - 24} more" if len(no_adj) > 24 else "")
                             + (f"; {lost} of {len(chunks)} map graph reads did not answer" if lost else
                                "; not in the bridge's baked map graph"))
        if any(g.get("truncated") for g in graph_raws):
            unread["border"] = "the map graph stopped at its node cap: some border counties are not listed"
    region = set(keys)
    border = sorted(k for k in graph if k not in region and (graph[k].get("kingdom") != res.get("title_key")))
    # border holders: their realm ruler and its strength
    bh = sorted({graph[k]["holder"] for k in border if _i(graph[k].get("holder"))})[:MAX_BORDER_HOLDERS]
    chars = await gw.read_many([("query_character", {"character_id": h}) for h in bh]) if bh else []
    bchar = {h: _d(c) for h, c in zip(bh, chars)}
    top_of = {h: (_i(c.get("top_liege_id")) or h) for h, c in bchar.items() if c}
    realms = {r["ruler_id"]: r for r in res.get("realms") or [] if isinstance(r, dict) and _i(r.get("ruler_id"))}
    extra_rulers = sorted({t for t in top_of.values() if t not in realms})
    if extra_rulers:
        batch = _d(await gw.read("query_realm", {"character_ids": extra_rulers}))
        for rr in batch.get("realms") or []:
            if isinstance(rr, dict) and _i(rr.get("character_id")):
                realms[rr["character_id"]] = {"ruler_id": rr["character_id"], "ruler_name": None,
                                              "military_strength": _i(rr.get("military_strength"))}
    names = {h: c.get("name") for h, c in bchar.items() if c.get("name")}
    for r in rows:
        for k, n in (("holder_id", "holder_name"), ("realm_ruler_id", "realm_ruler_name")):
            if _i(r.get(k)) and r.get(n):
                names[r[k]] = r[n]
    my_str = _i(me_realm.get("military_strength")) or _i((realms.get(me) or {}).get("military_strength"))
    # cb layer: the casus belli the cbs read gives against each realm ruler
    cb_by_ruler: dict = {}
    if "cb" in lay:
        targets = sorted({x for x in [r.get("realm_ruler_id") for r in rows] + list(top_of.values())
                          if _i(x) and x != me})[:MAX_CB_REALMS]
        cb_by_ruler = await R.declarable_now(gw, me, targets) if targets else {}
        if any(v is None for v in cb_by_ruler.values()):
            unread["cb"] = "the casus-belli read did not answer for some realm rulers (cb=?)"
    # mil layer
    goals: dict = {}
    sieges_by_county: dict = {}
    armies_by_county: dict = {}
    war_lines: list = []
    if want_mil:
        wars = [w for w in wars_raw.get("wars") or [] if isinstance(w, dict)]
        if not isinstance(wars_raw.get("wars"), list):
            unread["mil"] = "query_wars did not answer: your wars and their sieges are unread"
        by_id = {r.get("title_id"): r.get("key") for r in rows}
        sieges = [s for w in wars for s in (w.get("sieges") or []) if isinstance(s, dict)]
        arm = [a for a in armies_raw.get("armies") or [] if isinstance(a, dict) and (a.get("troop_count") or 0) > 0]
        bn = await _barony_counties(gw, sorted({s["holding_title_id"] for s in sieges if _i(s.get("holding_title_id"))}),
                                    sorted({a["location_barony"] for a in arm if a.get("location_barony")}))
        for w in wars:
            tt = [by_id.get(t) for t in w.get("target_titles") or [] if by_id.get(t)]
            for k in tt:
                goals[k] = w.get("war_id")
            side = "attacker" if w.get("is_attacker") else "defender"
            foe = w.get("primary_defender") if w.get("is_attacker") else w.get("primary_attacker")
            war_lines.append(f"# war {w.get('war_id')} you {side} vs {foe} for "
                             f"{','.join(handle(k) for k in tt) or 'a title outside the region'}: score "
                             f"{w.get('war_score') if w.get('war_score') is not None else '?'}")
        for s in sieges:
            b = bn.get(s.get("holding_title_id"))
            pct = s.get("progress_pct")
            cell = (f"siege {handle(b[0]) if b else s.get('holding_title_id')}:"
                    f"{int(round(pct)) if isinstance(pct, (int, float)) else '?'}%:{s.get('status', '?')}:"
                    f"{s.get('besieger_id', '?')}")
            sieges_by_county.setdefault(b[1] if b else None, []).append(cell)
        for a in arm:
            b = bn.get(a.get("location_barony"))
            cell = f"army {a.get('army_id')}:{a.get('troop_count')}@{handle(a.get('location_barony'))}"
            armies_by_county.setdefault(b[1] if b else None, []).append(cell)
        lost = (sieges_by_county.pop(None, []) + armies_by_county.pop(None, []))
        if lost:
            unread["mil_outside"] = f"{len(lost)} of your armies or sieges stand outside the region or unread: " \
                                    + "; ".join(lost[:4])
        # Other rulers' armies standing in
        # the region's counties (query_armies others_in_counties), each on its county's row
        o_ids = [str(r["title_id"]) for r in rows if _i(r.get("title_id"))]
        o_chunks = [c.split(",") for c in county_chunks(o_ids)] if o_ids else []
        o_chunks = [c[i:i + OTHERS_IDS_MAX] for c in o_chunks for i in range(0, len(c), OTHERS_IDS_MAX)]
        o_got = await gw.read_many([("query_armies", {"character_id": me, "others_in_counties": ",".join(c)})
                                    for c in o_chunks]) if o_chunks else []
        key_of = {r.get("title_id"): r.get("key") for r in rows}
        o_lost = 0
        for g in o_got:
            g = _d(g)
            if not isinstance(g.get("others"), list):
                o_lost += 1
                continue
            for o in g["others"]:
                k = key_of.get(o.get("county_id")) if isinstance(o, dict) else None
                if k:
                    armies_by_county.setdefault(k, []).append(
                        f"other {o.get('army_id', '?')}:{o.get('men', '?')}@{handle(o.get('barony')) if o.get('barony') else '?'}"
                        f" by {o.get('owner_id')}")
            if _i(g.get("others_total")) and _i(g.get("others_total")) > len(g["others"]):
                o_lost += 1
        if o_lost or not o_chunks:
            unread["other_armies"] = (f"other rulers' armies: {o_lost} of {len(o_chunks)} reads did not answer or "
                                      "were cut (an older bridge has no others_in_counties)" if o_chunks else
                                      "other rulers' armies: no county of the region was read by id")
        unread["other_armies_border"] = "other rulers' armies in the border counties (out) are not read"

    def cb_cell(ruler, county_id) -> str:
        if not _i(ruler) or ruler == me:
            return ""
        if ruler not in cb_by_ruler:
            return "cb=?"
        opts = cb_by_ruler.get(ruler)
        if opts is None:
            return "cb=?"
        mine = []
        for o in opts:
            if not (o.get("valid") and o.get("dispatch_supported")):
                continue
            t = _i(o.get("cb_title_id")) or 0
            if county_id is not None and t != county_id:
                continue
            if county_id is None and t:
                continue
            n = _CB_SHORT.get(o.get("cb"), str(o.get("cb") or "?").removesuffix("_cb"))
            mine.append(n + ("!" if o.get("blocked_by") else ""))
        return "cb=" + (",".join(sorted(set(mine))) or "-")

    def adj_cell(key) -> str:
        g = graph.get(key)
        if not g:
            return "adj=?"
        return "adj=" + ",".join(_SIGIL.get({"sea": "s", "river": "r", "impassable": "x"}.get(t, ""), "") + handle(n)
                                 for n, t in sorted(g["edges"].items()))

    if "land" in lay and graph and not any(g.get("terrain") for g in graph.values()):
        unread["terrain"] = "the baked map graph carries no county terrain (land layer: last field ?)"

    def land_cell(key) -> str:
        g = graph.get(key) or {}
        return "/".join(str(g.get(f)) if g.get(f) is not None else "?" for f in ("dev", "ctl", "faith", "culture",
                                                                                  "terrain"))

    def row_line(key, holder, ruler, yours, county_id, duchy, duchy_yours, out=False) -> str:
        cols = [handle(key)]
        for x in lay:
            if x == "holders":
                if yours:
                    cols.append("YOU")
                elif not _i(holder):
                    cols.append("?")
                else:
                    s = f"{holder}:{names.get(holder) or '?'}"
                    if _i(ruler) and ruler != holder:
                        s += f"/{ruler}"
                    cols.append(s)
            elif x == "cb" and not yours:
                cols.append(cb_cell(ruler, county_id) if county_id is not None or out else "cb=?")
            elif x == "mil":
                cols += (["WAR"] if key in goals else []) + armies_by_county.get(key, []) + sieges_by_county.get(key, [])
            elif x == "str" and not yours and _i(ruler) and _i((realms.get(ruler) or {}).get("military_strength")) is None:
                cols.append("str=?")
            elif x == "adj":
                cols.append(adj_cell(key))
            elif x == "duchy":
                cols.append(f"d={handle(duchy) if duchy else '?'}{'*' if duchy_yours else ''}")
            elif x == "land":
                cols.append(land_cell(key))
        return " ".join(c for c in cols if c) + (" (out)" if out else "")

    duchy_mine = {d.get("key") for d in res.get("duchies") or [] if isinstance(d, dict) and d.get("you_hold_it")}
    lines = []
    for r in rows:
        yours = r.get("you_control") is True
        lines.append(row_line(r.get("key"), r.get("holder_id"), r.get("realm_ruler_id"), yours, r.get("title_id"),
                              r.get("duchy"), r.get("duchy") in duchy_mine))
    out_lines = []
    for k in border:
        h = graph[k].get("holder")
        out_lines.append(row_line(k, h, top_of.get(h), False, None, graph[k].get("duchy"), False, out=True))
    # the header
    summ = _d(res.get("summary"))
    gold = me_chr.get("gold")
    pr = _d(_d(cost).get("price"))
    price = pr.get("cost") if pr.get("known") is True else None
    head = [f"# {res.get('title_key')} {(gw.game() or {}).get('date') or '?'} you={me} str={my_str if my_str is not None else '?'}"
            f" counties {summ.get('held', '?')}/{summ.get('total', '?')} need {summ.get('creation_needs_counties') or '?'}"
            f" gold {round(gold, 1) if isinstance(gold, (int, float)) else '?'}/"
            f"{round(price) if isinstance(price, (int, float)) else '?'}"]
    head += war_lines
    if "str" in lay:
        rr = []
        for rid, rv in sorted(realms.items(), key=lambda kv: (kv[1].get("military_strength") or 0)):
            if rid == me:
                continue
            s = _i(rv.get("military_strength"))
            ratio = f" x{s / my_str:.1f}" if s is not None and my_str else ""
            realm_cb = cb_cell(rid, None) if "cb" in lay else ""
            # With the allies likely to answer his call (cbs' his_likely, one source)
            da = await R.defender_allies(gw, rid, attacker_id=me)
            al = _i(da.get("likely_strength")) if isinstance(da, dict) else None
            al_cell = (f" al{al}" if al is not None and s is not None and al != s else
                       " al?" if al is None else "")
            # the names are on the rows (holders layer); the realms line carries the numbers
            rr.append(f"{rid} {s if s is not None else '?'}{ratio}{al_cell}"
                      + (f" {realm_cb}" if realm_cb and realm_cb != "cb=-" else ""))
        head.append("# realms: " + "; ".join(rr))
    head.append("# " + "; ".join(LEGEND[x] for x in lay) + "; ?=unread (see unread); (out)=border")
    _LOOKED[me] = (gw.game() or {}).get("date")
    result = {"region": res.get("title_key"), "region_from": why_region, "layers": list(lay), "text": "",
              "calls": {"county": "inspect(subject='county', title_id='c_<handle>')",
                        "wars": "cbs(target_id=<realm ruler id>)",
                        "holders": f"inspect(subject='title', title_key='{res.get('title_key')}')"}}
    if unread:
        result["unread"] = unread
    _fit(result, head, lines, out_lines)
    return result, errors, None


def _fit(result: dict, head: list, lines: list, out_lines: list) -> None:
    """result["text"] from the lines, kept at MAP_RESULT_MAX bytes: the border lines cut from the end first, then
    the adjacency of the region's last rows (adj=cut); each cut counted in result["truncated"]."""
    import reply_fit as _rf
    n_out, n_adj = len(out_lines), 0
    adj_rows = [i for i, x in enumerate(lines) if " adj=" in x]

    def compose() -> None:
        body = list(lines)
        for i in adj_rows[len(adj_rows) - n_adj:] if n_adj else []:
            body[i] = re.sub(r" adj=\S+", " adj=cut", body[i])
        h = list(head)
        if n_adj:
            h[-1] += "; adj=cut: see truncated"
        result["text"] = "\n".join(h + body + out_lines[:n_out]) + "\n"
        tr = {}
        if n_out < len(out_lines):
            tr["border"] = {"shown": n_out, "of": len(out_lines)}
        if n_adj:
            tr["adj"] = {"shown": len(adj_rows) - n_adj, "of": len(adj_rows)}
        if tr:
            tr["why"] = "the reply's 8 KB; title_key=<a kingdom or duchy of it> reads that part whole"
            result["truncated"] = tr
    compose()
    while _rf.size(result) > MAP_RESULT_MAX and n_out > 0:
        n_out -= 1
        compose()
    while _rf.size(result) > MAP_RESULT_MAX and n_adj < len(adj_rows):
        n_adj += 1
        compose()


async def holders_now(gw, me: int, region: str) -> Optional[dict]:
    """{county key: [holder id, realm ruler id]} of the region, from one read (query_dejure_progress verbose)
    and the region's keys known from the last digest or title read; None when it did not answer."""
    prog = _d(await gw.read("query_dejure_progress", {"character_id": me, "verbose": 1, "title_key": region}))
    rows = prog.get("counties")
    if not isinstance(rows, list):
        return None
    return {r.get("county_id"): [r.get("holder_id"), r.get("top_liege_id")] for r in rows if isinstance(r, dict)}


async def news_part(gw, me: Any, date: Any, baseline: str) -> Optional[dict]:
    """observe news `map_changes`: the region's holders that changed since this baseline's previous news (each
    change once), with the map read to take. The region is the default one (your primary title's kingdom).
    None when nothing changed or nothing could be read; the first call only takes the baseline."""
    me = _i(me)
    if not me:
        return None
    key = (baseline, me)
    prev = _LAST.get(key)
    region = (prev or {}).get("region")
    if not region:
        region, _ = await default_region(gw, me)
        if not region:
            return None
    now = await holders_now(gw, me, region)
    if now is None:
        return None
    _LAST[key] = {"region": region, "date": date, "holders": now}
    if not prev or prev.get("region") != region:
        return None
    changed = [{"county": _KEYS.get(c) or c, "holder": f"{(prev['holders'].get(c) or [None])[0]} -> {h[0]}",
                **({"realm": f"{(prev['holders'].get(c) or [None, None])[1]} -> {h[1]}"}
                   if (prev["holders"].get(c) or [None, None])[1] != h[1] else {})}
               for c, h in now.items() if c in prev["holders"] and prev["holders"][c] != h]
    if not changed:
        return None
    # The counties whose realm was yours and is another's now (news.counties_left_realm names the cause)
    left = [{"title_id": c, "county": _KEYS.get(c) or c, "holder_was": (prev["holders"].get(c) or [None])[0],
             "holder_now": h[0], "realm_now": h[1]}
            for c, h in now.items() if c in prev["holders"] and (prev["holders"][c] or [None, None])[1] == me
            and h[1] != me]
    return {"since": prev.get("date"), "region": region, "holders_changed": changed,
            "your_last_map_look": _LOOKED.get(me) or "never",
            "next": {"tool": "inspect", "args": {"subject": "map"}}, **({"_left": left} if left else {})}


async def one_county(gw, res: dict, title_id: int, me: int) -> dict:
    """inspect(subject='county', title_id=X): the county read answers every county of your realm
    whatever the title; this keeps the named one. A county outside your realm is answered from the
    map graph's live overlay (holder, development, control, faith, culture, terrain, neighbours) and
    says which read it came from; a title that is not a county is said so."""
    rows = [c for c in res.get("counties") or [] if isinstance(c, dict)]
    mine = [c for c in rows if _i(c.get("title_id")) == title_id]
    out = {k: v for k, v in res.items() if k not in ("counties", "county_count")}
    if mine:
        return {**out, "counties": mine, "county_count": 1, "of_your_counties": len(rows)}
    t = _d(_d(await gw.read("query_de_jure", {"title_id": title_id})).get("title"))
    key = t.get("key")
    if not key or _i(t.get("tier")) != 2:
        return {**out, "counties": [], "county_count": 0,
                "not_a_county": f"title {title_id} is {key or 'unread'} (tier {t.get('tier_name') or '?'}), "
                                "not a county"}
    g = _d(await gw.read("query_map_graph", {"character_id": me, "scope": "explicit", "counties": key,
                                             "hops": 1, "max_nodes": 1}))
    node = parse_graph(g.get("text")).get(key) if g.get("text") else None
    row = {"title_id": title_id, "key": key, "name": t.get("name"), "holder_id": t.get("holder_id"),
           "holder_name": t.get("holder_name"), "de_jure_parent": t.get("de_jure_parent_key"), "yours": False}
    if node:
        row.update({k: node.get(k) for k in ("terrain", "dev", "ctl", "faith", "culture") if node.get(k) is not None})
        row["source"] = "query_de_jure (holder) and the map graph's live overlay (dev, control, faith, culture)"
    else:
        row["unread"] = ("development, control, faith, culture: the map graph did not answer"
                         + (f" ({g.get('error')})" if g.get("error") else ""))
    return {**out, "counties": [row], "county_count": 1, "of_your_counties": len(rows),
            "note": "not a county of your realm: the county read gives only yours; this row is from other reads",
            "map": {"tool": "inspect", "args": {"subject": "map"}}}
