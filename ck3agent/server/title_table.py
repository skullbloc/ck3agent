"""inspect(subject="title"): one table of a de-jure title's counties.

A seat planning "form k_ireland" needs, in one call: every de-jure county with its
id, key and name, who holds it, whose realm it is in (the independent ruler at the
top of the holder's liege chain), how strong that realm is, whether the caller
controls it, and whether the caller has a claim on it; plus the same per de-jure
duchy and the bar the title's creation needs. Before this the subject answered
county ids and holder ids only.

Composed from reads the bridge already serves (no new bridge surface):
  span 1  query_dejure_progress (verbose: county_id, holder_id, top_liege_id,
          you_control, held/total, the meter create_title's own gate counts with),
          query_de_jure on the title (key, name, tier, its de-jure children),
          query_claims (the caller's claims).
  span 2  query_de_jure on each de-jure child above county tier (the counties'
          keys, names, holder names; for an empire, its kingdoms' duchies one more
          span down), query_realm batched over the realm rulers (military_strength,
          the realm's county count, its primary title).
  span 3  query_character for a ruler whose name no row above carried.
Every unread field is null with the read that did not answer named in errors[];
nothing is defaulted.
"""
from __future__ import annotations

import json
from typing import Any, Optional

#: The most de-jure-title reads one call walks (an empire's kingdoms and their
#: duchies). Past it the rows that were not read are counted, never dropped silently.
MAX_TITLE_READS = 48
#: The most realm rulers named by query_character in span 3.
MAX_NAME_READS = 16



#: The casus belli a holder of a county's de-jure duchy has on it, with no claim
#: (common/casus_belli_types/00_dejure_war.txt individual_county_de_jure_cb: 100 prestige times
#: the common multipliers; needs the culture innovation innovation_casus_belli, which the cbs
#: read answers once the duchy is held)
DEJURE_COUNTY_CB = "individual_county_de_jure_cb"

def _d(x) -> dict:
    return x if isinstance(x, dict) else {}


def _i(v) -> Optional[int]:
    return v if isinstance(v, int) and not isinstance(v, bool) else None


#: What each county row's next_call is, and whom a war names.
NEXT_CALL_RULE = ("next_call per county not yours: declare_war (declare_args) when the casus-belli "
                  "read cbs() answers says a war for that county is declarable now against its "
                  "realm ruler; else, without a claim, the court chaplain's fabricate-claim do call "
                  "(no read gives how long a fabrication takes); "
                  "else the gate. A war's target_id is the realm ruler (realm_ruler_id: the "
                  "independent ruler at the top, the holder himself when he is independent): "
                  "declare_war refuses a vassal (target_not_independent).")


#: (5 of 6 `inspect title k_ireland` replies went over the 8 KB cap and lost the two
#: explanations, or the county names, to the cut): what the seat's county rows leave out, said once.
SEAT_ROWS_NOTE = ("a county row's realm ruler's name and strength are in realms (ruler_id); a next_call that is a "
                  "name is a call in next_call_templates, with <title_id> the row's own title_id")
_REALM_REPEATS = ("realm_ruler_name", "realm_ruler_strength")


def seat_rows(result: dict) -> dict:
    """The title table as the seat reads it (inspect subject=title): each county row without the realm
    ruler's name and strength the realms list carries by ruler_id, and its next_call (the same call in every row but
    its title id) by the name of a template kept once in next_call_templates (reply_fit's own templating), so a
    kingdom's table fits the reply cap whole. A row whose realm ruler is not in realms keeps both fields. Other
    readers of title_table (map_view, fabricate_targets) keep the whole rows."""
    import reply_fit as _rf
    realms = {r.get("ruler_id") for r in result.get("realms") or [] if isinstance(r, dict)}
    out = dict(result)
    rows = [{k: v for k, v in r.items() if k not in _REALM_REPEATS or r.get("realm_ruler_id") not in realms}
            if isinstance(r, dict) else r for r in result.get("counties") or []]

    def shape(r: dict) -> str:                  # the call a row's next_call is, its own title id taken out
        scratch: dict = {}
        _rf._next_call_template(scratch)(dict(r))
        return json.dumps(scratch.get("next_call_templates"), sort_keys=True)
    shapes = [shape(r) if isinstance(r, dict) and isinstance(r.get("next_call"), dict) else None for r in rows]
    tpl = _rf._next_call_template(out)
    # only a call that repeats is named by a template; a call one row alone has stays in the row whole
    out["counties"] = [tpl(r) if sh is not None and shapes.count(sh) > 1 else r for r, sh in zip(rows, shapes)]
    out["county_rows"] = SEAT_ROWS_NOTE
    return out


def next_call_of(result: dict, row: dict):
    """A county row's next_call as a call: the row's own dict, or its named template from next_call_templates with
    the row's title id put in (the reverse of seat_rows' templating); None when the row has none."""
    nc = row.get("next_call")
    if not isinstance(nc, str):
        return nc
    tpl = (result.get("next_call_templates") or {}).get(nc)
    if tpl is None:
        return None
    return json.loads(json.dumps(tpl).replace('"<title_id>"', json.dumps(row.get("title_id"))))


async def _next_calls(gw, me: int, rows: list) -> None:
    """Each county row not yours gets `next_call`, judged by
    redesign.declarable_now, the read cbs() answers (one source of truth)."""
    import redesign as R
    todo = [r for r in rows if r.get("you_control") is not True and _i(r.get("realm_ruler_id"))
            and r.get("realm_ruler_id") != me]
    verdicts = await R.declarable_now(gw, me, [r["realm_ruler_id"] for r in todo]) if todo and me else {}
    for r in rows:
        if r.get("you_control") is True:
            continue
        ruler = _i(r.get("realm_ruler_id"))
        opts = verdicts.get(ruler) if ruler else None
        if opts is None:
            r["next_call"] = {"gate": "read_timeout", "summary": "the casus-belli read for its realm "
                                                                "ruler did not answer (or no ruler read)"}
            continue
        mine = [o for o in opts if _i(o.get("cb_title_id")) == r.get("title_id")]
        ok = next((o for o in mine if R.cb_option_declarable(o)), None)
        if ok is not None:
            r["next_call"] = {"tool": "declare_war", "args": {"declare_args": R.declare_args_of(ok, ruler)}}
        elif r.get("your_claim"):
            claim = next((o for o in mine if R.is_claim_cb_name(o.get("cb"))), None)
            blk = R.unmet_gate(claim) if claim else {"gate": "cb_invalid_for_target",
                                                       "summary": "no claim casus belli for this county "
                                                                  "in the game's list"}
            r["next_call"] = {"gate": blk.get("gate"), "summary": blk.get("summary")}
        else:
            r["next_call"] = {"tool": "do", "args": {"intent": "council_task",
                                                     "args": {"seat": R.FABRICATE_CLAIM_SEAT,
                                                              "task": "task_fabricate_claim",
                                                              "target_title_id": r.get("title_id")}}}


async def title_table(gw, *, character_id: int, title_key: str = "", title_id: int = 0,
                      next_calls: bool = True) -> dict[str, Any]:
    """The `result` and errors for inspect(subject="title"): (result, errors, blocked)."""
    import redesign as R
    errors: list = []
    if not title_key and not title_id:
        return {}, [], R.why_blocked("missing_required_argument",
                                     summary="name the title: title_key='k_scotland' (or title_id=<id>)")
    named = {"title_key": title_key} if title_key else {"title_id": title_id}
    prog, top, claims = await gw.read_many([
        ("query_dejure_progress", {"character_id": character_id, "verbose": 1, **named}),
        ("query_de_jure", dict(named)),
        ("query_claims", {"character_id": character_id}),
    ])
    prog, top, claims = _d(prog), _d(top), _d(claims)
    title = _d(top.get("title"))
    if str(prog.get("error") or "").startswith("title_key") or top.get("error") == "title_key_not_found":
        return {}, [], R.why_blocked("target_not_assignable", raw_reason="title_key_not_found",
                                     summary=f"the game has no landed title {title_key or title_id!r}")
    if prog.get("error"):
        errors.append({"code": "read_refused", "detail": f"query_dejure_progress: {prog.get('error')}"})
    if not title:
        errors.append({"code": "read_timeout", "detail": "query_de_jure (the title's key, name and "
                                                         "de-jure children) did not answer"})
    if not isinstance(claims.get("claims"), list):
        errors.append({"code": "read_timeout", "detail": "query_claims did not answer: `your_claim` is null"})
    claim_by_tid = {c.get("title_id"): c for c in (claims.get("claims") or []) if isinstance(c, dict)}
    claims_read = isinstance(claims.get("claims"), list)

    # De-jure children above county tier, walked down to the counties.
    county_meta: dict = {}          # county id -> {key, name, holder_name, duchy}
    duchies: list = []
    reads = 0
    unread_titles = 0
    level = [(c, None) for c in (top.get("de_jure_children") or []) if isinstance(c, dict)]
    if _i(title.get("tier")) == 2:          # the title is itself a county
        county_meta[title.get("title_id")] = {"key": title.get("key"), "name": title.get("name"),
                                              "holder_name": title.get("holder_name"), "duchy": None}
        level = []
    while level:
        nxt = []
        counties_here = [(c, parent) for c, parent in level if _i(c.get("tier")) == 2]
        for c, parent in counties_here:
            county_meta[c.get("title_id")] = {"key": c.get("key"), "name": c.get("name"),
                                              "holder_name": c.get("holder_name"), "duchy": parent}
        above = [(c, parent) for c, parent in level if (_i(c.get("tier")) or 0) > 2]
        room = max(0, MAX_TITLE_READS - reads)
        unread_titles += max(0, len(above) - room)
        above = above[:room]
        reads += len(above)
        replies = await gw.read_many([("query_de_jure", {"title_id": c.get("title_id")}) for c, _ in above]) \
            if above else []
        for (c, parent), rep in zip(above, replies):
            rep = _d(rep)
            kids = [k for k in (rep.get("de_jure_children") or []) if isinstance(k, dict)]
            if _i(c.get("tier")) == 3:
                duchies.append({"title_id": c.get("title_id"), "key": c.get("key"), "name": c.get("name"),
                                "holder_id": c.get("holder_id"), "holder_name": c.get("holder_name"),
                                "_counties": [k.get("title_id") for k in kids if _i(k.get("tier")) == 2],
                                "_read": bool(rep)})
            if not rep:
                errors.append({"code": "read_timeout",
                               "detail": f"query_de_jure for {c.get('key') or c.get('title_id')} did not answer"})
            nxt += [(k, c.get("key") if _i(c.get("tier")) == 3 else parent) for k in kids]
        level = nxt
    if unread_titles:
        errors.append({"code": "row_cap", "detail": f"{unread_titles} de-jure titles were not walked "
                                                    f"(the walk stops at {MAX_TITLE_READS} reads); their "
                                                    "counties lack key, name and duchy"})

    rows = [r for r in (prog.get("counties") or []) if isinstance(r, dict)]
    rulers = sorted({r.get("top_liege_id") for r in rows if _i(r.get("top_liege_id"))})
    realm_rows: dict = {}
    if rulers:
        batch = _d(await gw.read("query_realm", {"character_ids": rulers}))
        for rr in batch.get("realms") or []:
            if isinstance(rr, dict) and _i(rr.get("character_id")):
                realm_rows[rr["character_id"]] = rr
        if not isinstance(batch.get("realms"), list):
            errors.append({"code": "read_timeout", "detail": "the batched query_realm of the realm rulers "
                                                             "did not answer: ruler strength is null"})
    names: dict = {}
    for r in rows:
        meta = county_meta.get(r.get("county_id")) or {}
        if _i(r.get("holder_id")) and meta.get("holder_name"):
            names[r["holder_id"]] = meta["holder_name"]
    if _i(title.get("holder_id")) and title.get("holder_name"):
        names[title["holder_id"]] = title["holder_name"]
    for d in duchies:
        if _i(d.get("holder_id")) and d.get("holder_name"):
            names[d["holder_id"]] = d["holder_name"]
    unnamed = [x for x in rulers if x not in names][:MAX_NAME_READS]
    if unnamed:
        got = await gw.read_many([("query_character", {"character_id": x}) for x in unnamed])
        for x, g in zip(unnamed, got):
            g = _d(g)
            if g.get("name"):
                names[x] = g["name"]

    me = _i(prog.get("character_id")) or character_id
    out_rows = []
    for r in rows:
        cid = r.get("county_id")
        meta = county_meta.get(cid) or {}
        ruler = _i(r.get("top_liege_id"))
        rr = realm_rows.get(ruler) or {}
        claim = claim_by_tid.get(cid)
        out_rows.append({
            "title_id": cid, "key": meta.get("key"), "name": meta.get("name"), "duchy": meta.get("duchy"),
            "holder_id": r.get("holder_id"), "holder_name": names.get(r.get("holder_id")),
            "realm_ruler_id": ruler or None, "realm_ruler_name": names.get(ruler),
            "realm_ruler_strength": _i(rr.get("military_strength")),
            "you_control": r.get("you_control"),
            "your_claim": (None if not claims_read else
                           (claim.get("type") or ("pressed" if claim.get("pressed") else "unpressed"))
                           if claim else False),
        })
    if next_calls:
        await _next_calls(gw, me, out_rows)
    by_county = {r["title_id"]: r for r in out_rows}
    duchy_rows = []
    for d in duchies:
        cs = [by_county[c] for c in d["_counties"] if c in by_county]
        duchy_rows.append({"title_id": d["title_id"], "key": d["key"], "name": d["name"],
                           "holder_id": d["holder_id"], "holder_name": names.get(d["holder_id"]),
                           "you_hold_it": d["holder_id"] == me if _i(d.get("holder_id")) else False,
                           "counties": len(d["_counties"]) if d["_read"] else None,
                           "counties_you_control": sum(1 for c in cs if c.get("you_control") is True),
                           "your_claim": (None if not claims_read else bool(claim_by_tid.get(d["title_id"])))})
        # A duchy title you hold gives the game's de-jure casus belli on its counties
        # held outside your realm, with no fabricated claim (individual_county_de_jure_cb: its
        # valid_to_start wants you or your liege holding the county's de-jure duchy or above)
        others = [c.get("key") for c in cs if c.get("you_control") is False]
        if others and not duchy_rows[-1]["you_hold_it"] and duchy_rows[-1]["counties_you_control"]:
            duchy_rows[-1]["if_you_hold_it"] = {"no_claim_needed_on": others, "cb": DEJURE_COUNTY_CB}
    realms = []
    for ruler in rulers:
        rr = realm_rows.get(ruler) or {}
        realms.append({"ruler_id": ruler, "ruler_name": names.get(ruler),
                       "primary_title": rr.get("primary_title_key"),
                       "military_strength": _i(rr.get("military_strength")),
                       "realm_counties": _i(rr.get("realm_total_counties")),
                       "counties_here": sum(1 for r in out_rows if r["realm_ruler_id"] == ruler)})
    realms.sort(key=lambda x: -(x["counties_here"] or 0))
    held, total = _i(prog.get("held")) or 0, _i(prog.get("total")) or 0
    tkey = title.get("key") or prog.get("title_key") or title_key
    tier = _i(title.get("tier")) or R.title_tier_of_key(tkey)
    need = R.dejure_need(total, tier) if (tier or 0) >= R.MINT_DEJURE_MIN_TIER else None
    result = {
        "title_key": tkey, "title_id": title.get("title_id") or prog.get("title_id"),
        "name": title.get("name"), "tier": title.get("tier_name"),
        "holder_id": title.get("holder_id"), "holder_name": names.get(title.get("holder_id")),
        # held / total: the meter's own names (query_dejure_progress), read by a
        # harness's dejure_counties metric.
        "summary": {"counties_you_control": held, "counties": total, "held": held, "total": total,
                    "fraction": round(float(prog.get("fraction") or 0.0), 3),
                    "creation_needs_counties": need,
                    "duchies_you_hold": sum(1 for d in duchy_rows if d["you_hold_it"]),
                    "duchies": len(duchy_rows)},
        "counties": out_rows,
        "duchies": duchy_rows,
        "realms": realms,
        "strength_unit": "military_strength: the game's own soldier-strength figure for the ruler's "
                         "realm (levies plus men-at-arms), comparable between rows",
        "next_call_rule": NEXT_CALL_RULE,
    }
    if not rows:
        result["counties_note"] = (prog.get("note") or "query_dejure_progress listed no counties")
    if need is not None:
        result["creation_rule"] = R.MINT_DEJURE_RULE
        if next_calls and title.get("holder_id") != me and tkey:
            result["creation"] = await _creation(gw, me, tkey, prog, duchy_rows, out_rows)
    return result, errors, None


async def _creation(gw, me: int, tkey: str, prog: dict, duchy_rows: list, county_rows: list) -> dict:
    """The title's creation requirements in one block, from the function
    do(action_class="formable_titles") uses (redesign.title_requirements, with
    attach_tier_bill): counties held/needed, the tier rule, the price and the bill, and
    the nearest unmet requirement with its ready call. Reads: the game's create-title
    check for the title and for the one duchy nearest to creatable (the de-jure bar
    met, not yours), in one span; the counties come from this table's own read.
    Live 1068.11.27 the seat kept 229.67 gold rather than create d_meath (2/2 held)
    because "the duchy title doesn't add any counties": the kingdom's tier rule asks
    a duke for 2 duchy titles."""
    import redesign as R
    # The duchies priced are the ones the tier rule needs, nearest first (most
    # of their counties held), whether or not their de-jure bar is met: the same rows
    # do(action_class="formable_titles") prices when the seat names them, so both bills
    # agree (live 1073.02.08 this block read "unread: 1 priced" while formable_titles
    # priced both duchies of the rule).
    near = [d for d in duchy_rows if not d.get("you_hold_it") and isinstance(d.get("counties"), int)
            and d["counties"] > 0]
    near.sort(key=lambda d: (-d["counties_you_control"], d["counties"] - d["counties_you_control"],
                             d.get("key") or ""))
    cand = [d for d in near if d["counties_you_control"] >= R.dejure_need(d["counties"], 3)]
    duchy = cand[0] if cand else None
    (chk,) = await gw.read_many([("query_create_title_cost", {"actor_id": me, "title_key": tkey})])
    row, blk = await R.title_requirements(gw, me, tkey, prog=prog, chk=_d(chk))
    row["blocked"] = blk
    rows = [row]
    tr0 = next((q for q in row.get("requirements") or [] if q.get("requirement") == "tier_rule"), {})
    d_opt = next((o for o in tr0.get("one_of") or [] if o.get("tier") == "duchy"), None)
    short = max((d_opt["need"] - d_opt["have"]) if d_opt else 0, 1 if duchy else 0)
    priced = ([duchy] if duchy else []) + [d for d in near if d is not duchy][:max(short - (1 if duchy else 0), 0)]
    got = await gw.read_many([("query_create_title_cost", {"actor_id": me, "title_key": d["key"]})
                              for d in priced])
    drows = {}
    for d, c in zip(priced, got):
        n = d["counties"]
        drow, dblk = await R.title_requirements(gw, me, d["key"], chk=_d(c), prog={
            "held": d["counties_you_control"], "total": n, "fraction": d["counties_you_control"] / n})
        drow["blocked"] = dblk
        rows.append(drow)
        drows[d["key"]] = drow
    R.attach_tier_bill(rows)
    req = {q["requirement"]: q for q in row.get("requirements") or []}
    out: dict = {"source": "the requirements do(action_class='formable_titles') gives for this title"}
    dj = req.get("de_jure_counties")
    if dj:
        out["counties"] = {"have": dj["have"], "need": dj["need"], "met": dj["met"]}
    tr = req.get("tier_rule") or {}
    out["tier_rule"] = {k: tr[k] for k in ("met", "rule", "unread", "bill") if k in tr}
    pr = req.get("price") or {}
    out["price"] = {k: pr[k] for k in ("met", "cost", "currency", "have", "unread", "months_of_income",
                                       "income_basis") if k in pr}
    bill = (out.get("tier_rule") or {}).get("bill") or {}
    if pr.get("met") is False and bill.get("currency") == "gold" and isinstance(bill.get("cost"), (int, float)) \
            and isinstance(pr.get("months_of_income"), (int, float)) and isinstance(pr.get("have"), (int, float)):
        # The whole bill (the duchies the tier rule needs and this title) in months of income
        per = (pr["cost"] - pr["have"]) / pr["months_of_income"] if pr["months_of_income"] else None
        if per:
            out["price"]["bill_months_of_income"] = round((bill["cost"] - pr["have"]) / per)
    gc = req.get("game_check") or {}
    out["game_check"] = {k: gc[k] for k in ("met", "reasons", "unread") if k in gc}
    # the nearest unmet requirement, and the call that meets it
    # The duchies priced here, read by the goal line's own reading of a chain title (redesign.chain_step)
    steps = R.rank_chain([R.chain_step(r) for r in drows.values()])
    best = next((st for st in steps if st.get("state") != "lacks"), None)
    if tr.get("met") is False and duchy:
        key = best["title_key"] if best else duchy["key"]
        d = drows[key]
        st = best or R.chain_step(d)
        drow = next((x for x in duchy_rows if x.get("key") == key), None) or {}
        dprice = d.get("price") or {}
        out["nearest_unmet"] = {
            "requirement": "tier_rule", "step": f"create {key}",
            **({"counties": f"{drow['counties_you_control']}/{drow['counties']} held"}
               if isinstance(drow.get("counties"), int) else {}),
            "cost": dprice.get("cost"), "currency": dprice.get("currency"),
            "you_have": dprice.get(dprice.get("currency")) if dprice else None,
            "gate": (d.get("blocked") or {}).get("gate"), "state": st.get("state"),
            **({"lacks": st["lacks"]} if st.get("lacks") else {}),
            "next_call": {"tool": "do", "args": {"intent": "mint_title", "args": {"title_key": key}}}}
    elif dj and dj["met"] is False:
        call = next((r["next_call"] for r in county_rows
                     if isinstance(r.get("next_call"), dict) and r["next_call"].get("tool")), None)
        out["nearest_unmet"] = {"requirement": "de_jure_counties", "step": f"{dj['need'] - dj['have']} more counties",
                                **({"next_call": call} if call else {})}
    elif tr.get("met") is False:
        out["nearest_unmet"] = {"requirement": "tier_rule", "step": tr.get("rule"),
                                "detail": "no de-jure duchy here has its counties held enough to create"}
    elif pr.get("met") is False:
        out["nearest_unmet"] = {"requirement": "price", "step": f"{pr.get('cost')} {pr.get('currency')}",
                                "you_have": pr.get("have")}
    elif blk is None:
        out["next_call"] = {"tool": "do", "args": {"intent": "mint_title", "args": {"title_key": tkey}}}
    return out
