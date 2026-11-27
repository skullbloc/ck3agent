"""A reply's size, kept at what a seat's client takes in a turn.

The pre-flight at two starts (Munster 1066, West Francia 867) recorded first replies of 9-42 KB on observe, plan_war,
succession_plan, inspect and do: a king's realm (30 vassals, 117 counties) made every list long. `fit` takes a whole
reply envelope and, only when it is over CAP bytes (compact JSON, as the seat's shim sends it), shapes it in this
order, stopping as soon as it fits:

1. ranks the rows of the lists a decision reads (the counties still missing for a title first, the least loyal
   vassals first, what is available before what is not),
2. drops the fields a row repeats from elsewhere in the same reply (names beside ids, a refusal's prose beside its
   gate), and long explanations a seat has read before,
3. replaces whole blocks another call reads in full with that call,
4. cuts rows from the end of each list, the ones a decision needs last; a row a decision needs (`keep`) is cut only
   when nothing else is left, and then its id is listed in `not_shown`,
5. as a last resort, replaces the largest remaining value with its size and the call that reads it.

`truncated` (in `result`) says what was done: each list's `shown` and `of`, the rows not shown that a decision needs,
the blocks dropped and where they are read, the reply's size before, and the argument that narrows the read. A reply
under the cap is returned unchanged (the same object).
"""
from __future__ import annotations

import json
from typing import Any, Callable, Optional

CAP = 8192
#: A list is
#: never cut below this many rows by the row cuts; its count stays in `truncated.lists` (shown, of). A whole block goes
#: only by name, with the call that reads it (the `drop` step, or the last resort's `dropped` row).
MIN_ROWS = 1
#: A cut list's next row goes in, itself fit, only when at least this many bytes are left for it
PARTIAL_ROW_MIN = 400
#: the bytes a partial row's place and its list entry's `last_row_cut` take beyond the row
PARTIAL_ROW_OVERHEAD = 40


def size(o: Any) -> int:
    """The bytes the seat's shim sends (mcp_shim: json.dumps(payload, default=str))."""
    return len(json.dumps(o, default=str).encode("utf-8"))


def call_key(tool: str, args: Optional[dict]) -> str:
    """`tool:<subject|action_class|intent|what>`, the key SPECS is read by."""
    a = args or {}
    sub = a.get("subject") or a.get("action_class") or a.get("intent") or a.get("what") or ""
    return f"{tool}:{sub}" if sub else tool


def _f(v) -> float:
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else 0.0


def _not_mine(r: dict) -> bool:
    return isinstance(r, dict) and r.get("you_control") is not True


def _blocked_brief(row: dict, _res: Any = None) -> dict:
    """A row whose refusal carries prose beside its gate keeps the gate and the summary."""
    b = row.get("blocked")
    if isinstance(b, dict):
        row["blocked"] = {k: b[k] for k in ("gate", "summary") if k in b}
    gc = row.get("game_check")
    if isinstance(gc, dict):
        row["game_check"] = {k: gc[k] for k in ("sendable", "answer", "accept_score") if k in gc}
    return row


def _drop_zero(field: str) -> Callable[..., dict]:
    def f(row: dict, _res: Any = None) -> dict:
        v = row.get(field)
        if isinstance(v, dict):
            row[field] = {k: x for k, x in v.items() if x not in (0, None, False, "")}
        return row
    return f


#: (a pre-flight run 867: query vassal_loyalty sent 21 of Charles II's 30 vassals, 10 KB with every
#: contract's eleven obligations, most at 0): the contract's obligations above level 0, the rest said once.
CONTRACT_LEVELS_NOTE = "contract: the obligations above level 0; an obligation not listed is at level 0"


def compact_vassal_loyalty(reply: Any) -> Any:
    """query_vassal_loyalty's reply with each row's contract at its non-zero obligations and the rule said once
    (contract_levels); a reply without vassal rows unchanged."""
    if not isinstance(reply, dict) or not isinstance(reply.get("vassals"), list):
        return reply
    out = dict(reply)
    out["vassals"] = [_drop_zero("contract")(dict(r)) if isinstance(r, dict) else r for r in reply["vassals"]]
    out["contract_levels"] = CONTRACT_LEVELS_NOTE
    return out


_COUNTY_REPEATS = ("name", "holder_name", "realm_ruler_name", "realm_ruler_strength")


def _county_row(row: dict, res: dict) -> dict:
    """A county row without the names and strength the realms list carries, its next_call by template name."""
    row = {k: v for k, v in row.items() if k not in _COUNTY_REPEATS}
    return _next_call_template(res)(row)


def _goal_row(row: Any) -> bool:
    """A formable_titles row of the task's goal title (CK3_GOAL_TITLE, seat_events.GOAL_TITLE_ENV)."""
    import os  # noqa: PLC0415
    k = (os.environ.get("CK3_GOAL_TITLE") or "").strip()
    return bool(k) and isinstance(row, dict) and row.get("title_key") == k


def _law_row(row: dict, _res: Any = None) -> dict:
    """A law row's label and do_call say the same as its group, current law and law."""
    law = ((row.get("do_call") or {}).get("args") or {}).get("args", {}).get("law")
    out = {"group": row.get("group"), "current_law": row.get("current_law"), "law": law}
    for k in ("prestige_price", "blocked"):
        if row.get(k) is not None:
            out[k] = row[k]
    return out


def _plan_row(row: dict, res: dict) -> dict:
    """A recorded case (19 of 19 plan_war replies cut, rows 1.4 KB each at a 90-county realm): a plan_war row with
    its cb_options dropped (cbs target_id=<id> reads them) and its precheck's two sentences, the same in every row,
    said once for the reply (`precheck_rule`), so more rows fit before any is cut."""
    out = {k: v for k, v in row.items() if k != "cb_options"}
    pc = out.get("precheck")
    if isinstance(pc, dict):
        res.setdefault("precheck_rule", {k: pc[k] for k in ("limit", "not_checked") if k in pc})
        out["precheck"] = {k: v for k, v in pc.items() if k not in ("limit", "not_checked")}
    return out


#: Per call: `rank` {list path: sort key}, `compact` {list path: row function or fields to drop}, `explain` paths
#: of long explanations, `drop` [(path, the call that reads it)], `cut` [(list path, keep predicate or None)] in
#: the order lists are cut, `ids` {list path: the row field that names a row}, `keep` [paths a cut leaves whole
#: while anything else can go: the crown-loss news omitted with half the cap unused],
#: `narrow` (the arguments that narrow the read). A path is a dotted key under `result`.
SPECS: dict = {
    "observe": {
        # The titles lost, the highest tier first (k_france among 40 counties and baronies)
        "rank": {"news.titles_lost": lambda r: ("ekdcb".find(str(r.get("key") or "x")[:1]) % 6,)},
        "explain": ["money.breakdown_absent", "money.expenses_absent", "money.unit", "standing.monthly_note",
                    "standing.succession_eta_basis", "military.unit", "military.who_commands",
                    "standing_policies.holds", "standing_policies.not_offered", "since.withheld.note",
                    "succession.scope", "news.ai_spends.what"],
        "drop": [("posture", "steer (its read form)"), ("standing_policies", "steer (its read form)"),
                 ("council_brief", "inspect subject=council"), ("council", "inspect subject=council"),
                 ("military", "inspect subject=armies")],
        "cut": [("evaluations", None), ("news.events_answered", None)],
        "keep": list(("news." + k for k in ("wars_ended", "liege_changed", "titles_lost", "ruler_changed",
                                            "dynasty_head_changed", "rules_answered"))),
        "narrow": "depth=news reads the news alone; each dropped block names the call that reads it whole",
    },
    "plan_war": {
        "compact": {"targets": _plan_row},
        "cut": [("excluded", None), ("targets", None)],
        "ids": {"targets": "target_name"},
        "narrow": "max_results (rows by score), toward=<title key>; cbs target_id=<id> gives one target whole",
    },
    "succession_plan": {
        "explain": ["partition_forecast.rule", "partition_forecast.titles_by", "levers.take_out_of_split.vows_rule",
                    "levers.take_out_of_split.disinherit_rule", "death_estimate.source",
                    "succession_eta_basis", "levers.*.readback", "partition_forecast.excluded_children.why"],
        "drop": [("per_title_map", "inspect subject=title_succession title_id=<title>")],
        "cut": [("partition_forecast.excluded_children", None), ("primary_heir.keeps_titles", None)],
        "narrow": "inspect subject=heir_plan reads the heirs; inspect subject=title_succession one title",
    },
    "inspect:title": {
        "rank": {"counties": lambda r: (r.get("you_control") is True, not r.get("your_claim")),
                 "duchies": lambda r: (bool(r.get("you_hold_it")),
                                       r.get("counties_you_control") == r.get("counties"))},
        "compact": {"counties": _county_row, "duchies": ("name", "holder_name")},
        "explain": ["next_call_rule", "strength_unit"],
        "cut": [("counties", _not_mine), ("duchies", None)],
        "ids": {"counties": "key", "duchies": "key"},
        "narrow": ("title_key=<a kingdom or duchy of it> lists that part alone; inspect subject=map "
                   "title_key=<key> gives every county in one line"),
    },
    "inspect:vassals": {
        "rank": {"vassals": lambda r: (_f(r.get("opinion_of_me")), -_f(r.get("military_strength")))},
        "compact": {"vassals": _drop_zero("contract")},
        "cut": [("vassals", None)],
        "ids": {"vassals": "id"},
        "narrow": "query domain=vassal_loyalty, or inspect subject=character character_id=<id> for one",
    },
    "inspect:heir_plan": {"cut": [("heirs", None)], "ids": {"heirs": "id"},
                          "narrow": "succession_plan; inspect subject=character character_id=<id> for one heir"},
    "do:decisions": {"rank": {"unavailable": lambda r: ((r.get("blocked") or {}).get("gate") == "decision_not_shown",)},
                     "compact": {"unavailable": _blocked_brief, "available": _blocked_brief},
                     "cut": [("unavailable", None), ("available", None)],
                     "ids": {"unavailable": "label", "available": "label"},
                     "narrow": "candidates=[<decision keys>] reads only those"},
    "do:imprison_targets": {"rank": {"unavailable": lambda r: (-_f((r.get("game_check") or {}).get("accept_score")),)},
                            "compact": {"unavailable": _blocked_brief, "available": _blocked_brief},
                            "cut": [("unavailable", None), ("available", None)],
                            "ids": {"unavailable": "label", "available": "label"},
                            "narrow": "candidates=[<character ids>] reads only those"},
    "do:laws": {"rank": {"available": lambda r: (r.get("group") != "crown_authority",)},
                "compact": {"available": _law_row},
                "cut": [("available", None)], "ids": {"available": "law"},
                "narrow": ("each row is a law of a group the ruler has; do intent=change_law args.law=<law> changes "
                           "it; inspect subject=laws reads those in force")},
    # The goal title's row first, then rows
    # cut from the end, never inside the first row (the goal line reads that row's chain and at_death)
    "do:formable_titles": {"rank": {"unavailable": lambda r: (not _goal_row(r),),
                                    "available": lambda r: (not _goal_row(r),)},
                           "cut": [("unavailable", None), ("available", None)],
                           "narrow": "candidates=[<title keys>] reads only those"},
    "do:buildings": {"cut": [("available", None)], "narrow": "candidates=[<building keys>] looks up those"},
    "do:men_at_arms": {"cut": [("available", None)], "narrow": "candidates=[<men-at-arms types>] looks up those"},
    # The cheapest first, so a cut leaves the rows the purse is nearest to; the rows cut named by company id, with
    # the call that reads them
    "do:mercenaries": {"rank": {"available": lambda r: (not isinstance(r.get("price_estimate"), (int, float)),
                                                        _f(r.get("price_estimate")))},
                       "cut": [("available", None)], "ids": {"available": "company_id"},
                       "rest": {"available": lambda ids: {"tool": "do", "args": {"action_class": "mercenaries",
                                                                                  "candidates": ids}}},
                       "narrow": "candidates=[<company ids>] reads only those",
                       # a company's row is its price: a row cut inside would show a company with no price
                       "whole_rows": True},
    "discover:scheme_types": {"compact": {"scheme_types": ("is_basic", "max_breaches")},
                              "cut": [("scheme_types", None)], "ids": {"scheme_types": "scheme"},
                              "narrow": "discover what=scheme_targets scheme=<key> for one"},
}
DEFAULT_NARROW = "a narrower read: max_results, candidates, a title_key or a character_id where the call takes one"
#: A purse row (gold, prestige, piety) whose change or unexplained part is at least this size stays on the news'
#: first page, as `rules_answered` does: a fall of that size on a later page is a fall the seat may never read
PURSE_KEEP_MIN = 50.0
PURSE_KEEP_CURRENCIES = ("gold", "prestige", "piety")


def money_keep(res: Any) -> list:
    """The paths of the purse rows a cut or a page must keep on the first page (PURSE_KEEP_MIN): news.purse.<currency>
    for each currency whose |change| or |unexplained| is at least the threshold, and a ruler change's purse
    (news.purse.new_ruler) whatever its size."""
    purse = _value_at(res, "news.purse")
    if not isinstance(purse, dict):
        return []
    out = []
    if isinstance(purse.get("new_ruler"), dict):
        # A ruler change: the purse is the new ruler's own (no change row to size); it is kept whole, as is every
        # step kind's purse of that size
        out.append("news.purse.new_ruler")
    for cur in PURSE_KEEP_CURRENCIES:
        row = purse.get(cur)
        if not isinstance(row, dict):
            continue
        big = [abs(row[k]) for k in ("change", "unexplained")
               if isinstance(row.get(k), (int, float)) and not isinstance(row.get(k), bool)]
        if big and max(big) >= PURSE_KEEP_MIN:
            out.append("news.purse." + cur)
    return out


def keep_paths_of(spec: dict, res: Any, tool: str) -> list:
    """The spec's `keep` paths that hold something, and for observe the purse rows money_keep names."""
    keep = [kp for kp in spec.get("keep") or () if _value_at(res, kp) not in (None, [], {})]
    if tool == "observe":
        keep += [kp for kp in money_keep(res) if kp not in keep]
    return keep


def _get(root: Any, path: str):
    """(holder, key) of a dotted path under root, or (None, None) when it is not there."""
    parts = path.split(".")
    cur = root
    for p in parts[:-1]:
        if not isinstance(cur, dict) or not isinstance(cur.get(p), dict):
            return None, None
        cur = cur[p]
    return (cur, parts[-1]) if isinstance(cur, dict) and parts[-1] in cur else (None, None)


def _expand(root: Any, path: str) -> list:
    """Every (holder, key) a dotted path with `*` segments (any key of a dict) names under root."""
    found = [(None, root)]
    parts = path.split(".")
    for i, p in enumerate(parts):
        nxt = []
        for _h, cur in found:
            for c in (cur if isinstance(cur, list) else [cur]):     # a list: each of its rows
                if isinstance(c, dict):
                    nxt += [(c, k) for k in (list(c) if p == "*" else [p] if p in c else [])]
        if i < len(parts) - 1:
            found = [(h, h[k]) for h, k in nxt]
        else:
            return nxt
    return []


def _next_call_template(res: dict) -> Callable[[dict], dict]:
    """A county row's next_call, the same call for each row but its title id, becomes the name of a template kept
    once in result.next_call_templates (`<title_id>` where the row's own id goes)."""
    def f(row: dict) -> dict:
        nc, tid = row.get("next_call"), row.get("title_id")
        if not isinstance(nc, dict) or tid is None:
            return row
        text = json.dumps(nc, sort_keys=True)
        tpl = json.loads(text.replace(f": {tid}", ': "<title_id>"')) if f": {tid}" in text else nc
        args = (tpl.get("args") or {}) if isinstance(tpl, dict) else {}
        name = str(args.get("intent") or tpl.get("tool") or "call")
        sub = (args.get("args") or {}) if isinstance(args.get("args"), dict) else {}
        name = "_".join(str(x) for x in (name, sub.get("task") or sub.get("cb") or "") if x)
        store = res.setdefault("next_call_templates", {})
        while name in store and store[name] != tpl:
            name += "_"
        store[name] = tpl
        row["next_call"] = name
        return row
    return f


def _lists(o: Any, path: tuple = ()):
    items = o.items() if isinstance(o, dict) else enumerate(o)
    for k, v in items:
        if isinstance(v, list):
            if v:
                yield path + (k,), o, k, v
            yield from _lists(v, path + (k,))
        elif isinstance(v, dict):
            yield from _lists(v, path + (k,))


def sent(env: Any, tool: str, args: Optional[dict] = None) -> Any:
    """THE rule the server applies to a tool's reply (mcp_server calls this for each tool): a read is fit to CAP; a
    write's reply (do with an intent, declare_war, pass_turn, session) is sent whole; query fits its own replies
    first (mcp_server.fit_reply) and is not shaped again. With pages on (`pages_on`), a PAGED
    tool's reply is left whole here and paged once, in `delivered`."""
    a = args or {}
    if tool in ("query", "declare_war", "pass_turn", "session") or (tool == "do" and a.get("intent")):
        return env
    if paged_key(tool, a) and pages_on():
        return env
    return fit(env, tool, a)


def wrapper_bytes(tool: str) -> int:
    """A recorded case (nine replies of 8,199-8,630 bytes reached the seat, the cap having been applied to the
    toolkit's envelope before the host's tool-name prefix went into its hints and before the harness's seat shim
    wrapped it): the bytes the shim adds around a reply, {"tool_name": <tool>, "ok": .., "result": <reply>,
    "error": ..} (ToolResult.to_dict, json.dumps in mcp_shim)."""
    return size({"tool_name": tool, "ok": False, "result": None, "error": None}) - size(None)


def delivered(env: Any, tool: str, args: Optional[dict] = None, store: bool = True) -> Any:
    """THE cap on what the seat receives: the reply as it leaves the server (prefix applied), fit so that it and
    the shim's wrapper are at most CAP bytes. Writes are sent whole, as in `sent`; a read already shaped by `sent`
    is shaped again only if the prefix or the wrapper took it over, its earlier cuts kept in `truncated`."""
    a = args or {}
    if tool in ("declare_war", "pass_turn", "session") or (tool == "do" and a.get("intent")):
        return env
    k = paged_key(tool, a)
    if k and pages_on():
        return paginate(env, k, a, cap=CAP - wrapper_bytes(tool), store=store)
    return fit(env, tool, a, cap=CAP - wrapper_bytes(tool))


def fit(env: Any, tool: str, args: Optional[dict] = None, cap: int = CAP) -> Any:
    """The reply as sent: `env` itself when it is at most `cap` bytes, else a shaped copy (module docstring)."""
    if not isinstance(env, dict):
        return env
    try:
        first = size(env)
    except (TypeError, ValueError):
        return env
    if first <= cap:
        return env
    key = call_key(tool, args)
    spec = SPECS.get(key) or SPECS.get(tool) or {}
    out = json.loads(json.dumps(env, default=str))
    res = out["result"] if isinstance(out.get("result"), dict) else out
    block: dict = {"why": f"the reply was {first} bytes; at most {cap} are sent",
                   "narrow": spec.get("narrow") or DEFAULT_NARROW}
    lists: dict = {}
    dropped: list = []
    prior = res.pop("truncated", None)
    if isinstance(prior, dict):
        # a second pass (`delivered` after `sent`): the first pass's cuts stay said, its first size kept
        block["why"] = f"{prior.get('why') or block['why']}; {cap} after the host's prefix and wrapper"
        for r in prior.get("lists") or []:
            if isinstance(r, dict) and r.get("list"):
                lists[r["list"]] = dict(r)
        dropped.extend(x for x in prior.get("dropped") or [] if isinstance(x, dict))

    cut_full: dict = {}                            # path -> a cut list's rows (the regrow and partial steps)
    # Every row id a list that names its cut rows held when this reply was first shaped: its rows now, and the ids
    # an earlier pass cut (a second pass, after the host's prefix and wrapper, sees only the rows the first left)
    origin_ids: dict = {}
    origin_taken: list = []

    def origin() -> None:
        """Taken once, at the first finish (after the rank, before any cut)."""
        if origin_taken:
            return
        origin_taken.append(True)
        for path, idf in (spec.get("ids") or {}).items():
            if (spec.get("rest") or {}).get(path) is None:
                continue
            cur = _value_at(res, path)
            ids_now = [x.get(idf) for x in cur if isinstance(x, dict) and x.get(idf) is not None] \
                if isinstance(cur, list) else []
            was = [i for i in (lists.get(path) or {}).get("cut_ids") or [] if i not in ids_now]
            origin_ids[path] = ids_now + was

    def finish() -> dict:
        origin()
        b = dict(block)
        cut_ = [r for r in lists.values() if r.get("shown", 0) < r.get("of", 0)]
        for r in cut_:
            # Where the spec names a row's id and the call that reads named rows: how many rows are cut, which, and
            # the call that reads exactly those, on every path a cut takes (a row cut inside, last_row_cut, is among
            # them; the ids an earlier pass cut stay)
            every = origin_ids.get(r["list"])
            if every is None:
                continue
            idf = (spec.get("ids") or {}).get(r["list"])
            cur = _value_at(res, r["list"])
            whole = {x.get(idf) for x in cur if isinstance(x, dict) and "not_shown" not in x} \
                if isinstance(cur, list) else set()
            r["cut"] = r["of"] - r["shown"]
            got = [i for i in every if i not in whole]
            if got:
                r["cut_ids"] = got
                r["read_them"] = spec["rest"][r["list"]](got)
            else:
                r.pop("cut_ids", None)
                r.pop("read_them", None)
        if cut_:
            b["lists"] = cut_
        if dropped:
            b["dropped"] = dropped
        res["truncated"] = b
        return out

    def fits() -> bool:
        finish()
        return size(out) <= cap

    # 1. rank
    for path, k in (spec.get("rank") or {}).items():
        h, name = _get(res, path)
        if h is not None and isinstance(h[name], list):
            h[name] = sorted(h[name], key=lambda r: k(r) if isinstance(r, dict) else (True,))
    if fits():
        return out
    # 2. compact rows, then explanations
    gone = []
    for path in spec.get("explain") or []:
        hits = _expand(res, path)
        for h, name in hits:
            del h[name]
        if hits:
            gone.append(path)
    if gone:
        dropped.append({"what": "explanations: " + ", ".join(gone),
                        "read": "a reply of this call under the cap carries them"})
        if fits():
            return out
    for path, how in (spec.get("compact") or {}).items():
        h, name = _get(res, path)
        if h is None or not isinstance(h[name], list):
            continue
        rows = []
        for r in h[name]:
            if isinstance(r, dict):
                r = how(r, res) if callable(how) else {f: v for f, v in r.items() if f not in how}
            rows.append(r)
        h[name] = rows
        dropped.append({"what": f"{path}: fields a row repeats from this reply or that another call reads",
                        "read": block["narrow"]})
        if fits():
            return out
    # 3. blocks another call reads whole
    for path, where in spec.get("drop") or []:
        h, name = _get(res, path)
        if h is None:
            continue
        del h[name]
        dropped.append({"what": path, "read": where})
        if fits():
            return out
    # 4. rows from the end of each list: the spec's lists in order (rows no decision needs first), then any list
    ids = spec.get("ids") or {}
    full_rows: dict = {}                           # path -> a list's rows before a cut (step 6 regrows from them)
    keep_paths = tuple(keep_paths_of(spec, res, tool))

    def cut(holder, name, path, keep, protect: bool = True) -> bool:
        """Cut `path` to the most rows that fit; with `protect`, never below its leading `keep` rows. A `keep` row
        cut is named in not_shown. True when the reply fits."""
        rows = holder[name]
        if not isinstance(rows, list) or not rows:
            return False
        n = lists.get(path, {}).get("of", len(rows))
        floor = min(MIN_ROWS, len(rows))
        if keep is not None and protect:
            while floor < len(rows) and isinstance(rows[floor], dict) and keep(rows[floor]):
                floor += 1
        full = rows
        if keep is None:
            full_rows.setdefault(path, list(full))

        def apply(k: int) -> None:
            holder[name] = full[:k]
            row = {"list": path, "shown": k, "of": n}
            hidden = [r.get(ids[path]) for r in full[k:] if isinstance(r, dict) and ids.get(path)
                      and keep is not None and keep(r)]
            if hidden:
                row["not_shown"] = hidden
            if (spec.get("rest") or {}).get(path) is not None and ids.get(path):
                cut_full[path] = full
            lists[path] = row
        lo, hi = floor, len(full)
        apply(lo)
        if not fits():
            return False
        while lo < hi:                     # the most rows that fit
            mid = (lo + hi + 1) // 2
            apply(mid)
            if fits():
                lo = mid
            else:
                hi = mid - 1
        apply(lo)
        return True

    def partial_rows() -> None:
        """7. (cbs cut 12,519 -> 6,311 bytes, 1,881 under the cap, a row being 2,173) the room
        a cut leaves: the first row not shown of each cut list goes in with the fields that fit, in its own order
        (`partial_row`: the fields left out named in its `not_shown`), while the reply fits; one such row a list."""
        if spec.get("whole_rows"):
            return
        for path in sorted(every_rows, key=lambda p: (not _under(p, keep_paths), p)):
            rows, cur = every_rows[path], _value_at(res, path)
            row = lists.get(path)
            if (not isinstance(cur, list) or row is None or not cur or len(cur) >= len(rows)
                    or rows[:len(cur)] != cur or not isinstance(rows[len(cur)], dict)):
                continue
            holder_path, _, name = path.rpartition(".")
            holder = _value_at(res, holder_path) if holder_path else res
            room = cap - size(out) - PARTIAL_ROW_OVERHEAD
            for _ in range(4):
                if room < PARTIAL_ROW_MIN:
                    break
                part = partial_row(rows[len(cur)], room)
                holder[_key(holder, name)] = cur + [part]
                row["shown"] = len(cur) + 1
                row["last_row_cut"] = True
                if fits():
                    break
                over = size(out) - cap
                holder[_key(holder, name)] = cur
                row["shown"] = len(cur)
                row.pop("last_row_cut", None)
                room -= max(over, 0) + 16

    every_rows: dict = {}                          # path -> a cut list's rows before any cut (step 7 reads them)
    for path, keep in spec.get("cut") or []:
        h, name = _get(res, path)
        if h is not None and isinstance(h[name], list):
            every_rows.setdefault(path, list(h[name]))
        if h is not None and cut(h, name, path, keep):
            partial_rows()
            return finish()
    for path, keep in spec.get("cut") or []:      # then the rows a decision needs, their ids listed
        h, name = _get(res, path)
        if h is not None and keep is not None and cut(h, name, path, keep, protect=False):
            partial_rows()
            return finish()
    def halve(protect: bool) -> bool:
        """Any list, the largest first, halved, never below MIN_ROWS; with `protect`, never a `keep` path's.
        True when the reply fits."""
        for _ in range(256):
            if fits():
                return True
            found = max(((p, h, k, r) for p, h, k, r in _lists(res) if len(r) > MIN_ROWS and p[0] != "truncated"
                         and not (protect and _under(".".join(map(str, p)), keep_paths))),
                        key=lambda t: size(t[3]), default=None)
            if found is None:
                return False
            p, holder, k, rows = found
            path = ".".join(str(x) for x in p)
            full_rows.setdefault(path, list(rows))
            every_rows.setdefault(path, list(rows))
            keep_n = max(MIN_ROWS, len(rows) // 2)
            holder[k] = rows[:keep_n]
            lists[path] = {"list": path, "shown": keep_n, "of": lists.get(path, {}).get("of", len(rows))}
        return fits()

    # 5. (six cut observes sent under 6,000 bytes, the crown-loss one at 4,130 with its news
    # omitted whole) one value is replaced by its size: the SMALLEST whose place frees the room, else the largest,
    # then again; a `keep` path and the blocks holding one are never chosen while anything else is left
    omitted: list = []

    def omit(protect: bool) -> bool:
        for _ in range(64):
            if fits():
                return True
            over = size(out) - cap
            cands = []
            for path, holder, k, v in _values(res):
                if _under(path, ("truncated",)) or (isinstance(v, str) and v.startswith("omitted: ")):
                    continue
                if protect and (isinstance(v, list) or _under(path, keep_paths)
                                or any(_under(kp, (path,)) for kp in keep_paths)):
                    continue                       # a list keeps its first row (MIN_ROWS) while a block can go
                n = size(v)
                gain = n - size(f"omitted: {n} bytes")
                if gain > 0:
                    cands.append((gain, path, holder, k, v))
            if not cands:
                return False
            enough = [c for c in cands if c[0] >= over]
            gain, path, holder, k, v = min(enough, key=lambda c: c[0]) if enough else max(cands, key=lambda c: c[0])
            holder[k] = f"omitted: {size(v)} bytes"
            omitted.append((path, holder, k, v))
            dropped.append({"what": path, "read": block["narrow"]})
        return fits()

    if not (halve(True) or halve(False) or omit(True) or omit(False)):
        return finish()
    # 6. The room a cut left: the omitted values back (the smallest first), then the cut lists regrown (the
    # `keep` lists first), each only while the reply still fits
    def regrow(paths) -> None:
        for path in paths:
            rows = full_rows[path]
            cur = _value_at(res, path)
            if not isinstance(cur, list) or len(cur) >= len(rows) or rows[:len(cur)] != cur or path not in lists:
                continue
            holder_path, _, name = path.rpartition(".")
            holder = _value_at(res, holder_path) if holder_path else res
            lo, hi = len(cur), len(rows)
            while lo < hi:
                mid = (lo + hi + 1) // 2
                holder[_key(holder, name)] = rows[:mid]
                lists[path]["shown"] = mid
                if fits():
                    lo = mid
                else:
                    hi = mid - 1
            holder[_key(holder, name)] = rows[:lo]
            lists[path]["shown"] = lo

    for path, holder, k, v in sorted(omitted, key=lambda t: size(t[3])):
        if _value_at(res, path) is not holder.get(k):
            continue
        holder[k] = v
        if fits():
            dropped[:] = [d for d in dropped if d.get("what") != path]
        else:
            holder[k] = f"omitted: {size(v)} bytes"
    regrow(sorted(full_rows, key=lambda p: (not _under(p, keep_paths), p)))
    partial_rows()
    return finish()


def partial_row(row: dict, room: int) -> dict:
    """A row's fields, in its own order, while the row stays within `room` bytes; the fields left out are named
    in `not_shown` (a field too large for the room left is skipped, a later smaller one may still go in)."""
    out: dict = {}
    left: list = []
    for k, v in row.items():
        trial = {**out, k: v, "not_shown": left + ["x" * 24] * 3}
        if size(trial) <= room:
            out[k] = v
        else:
            left.append(k)
    out["not_shown"] = left
    return out


def _under(path: str, roots: tuple) -> bool:
    """`path` is one of `roots` or inside one."""
    return any(path == r or path.startswith(r + ".") for r in roots)


def _key(holder: Any, name: str):
    return int(name) if isinstance(holder, list) else name


def _value_at(root: Any, path: str) -> Any:
    """The value at a dotted path (a list's rows by index), or None."""
    cur = root
    for p in path.split(".") if path else []:
        if isinstance(cur, dict) and p in cur:
            cur = cur[p]
        elif isinstance(cur, list) and p.isdigit() and int(p) < len(cur):
            cur = cur[int(p)]
        else:
            return None
    return cur


def _values(o: Any, path: tuple = ()):
    """(dotted path, holder, key, value) of every dict value under o, at any depth (a list's rows by index)."""
    items = o.items() if isinstance(o, dict) else enumerate(o) if isinstance(o, list) else ()
    for k, v in items:
        p = path + (str(k),)
        if isinstance(o, dict):
            yield ".".join(p), o, k, v
        if isinstance(v, (dict, list)):
            yield from _values(v, p)


# ── Pages in place of cuts for observe, plan_war and succession_plan (and inspect
# subject=title and cbs, PAGED_CALLS) ──────────────────────────────────────────────────────────────────────────────
#
# A tool-shape audit: these three tools went over the cap at every realm size recorded (3 to 92 counties), and
# `fit` cut their lists mid-way (Francia's observe 20 of 20, plan_war 19 of 19), the rows cut gone from the turn and
# a `truncated` block (1-1.5 KB) said so. With pages on, a reply over the cap is laid out instead: its scalar fields
# (the summary) first, then its blocks in the tool's PAGE_ORDER, each whole on the first page with room for it; a
# list longer than a page continues on the next page, its rows whole and in the reply's own order (plan_war: by
# score). Nothing is dropped and nothing is cut inside a row: every row is on some page. The first page ends with
# `more`: the pages there are, the call that reads the next one (`inspect subject=more`, the one listing entry this
# adds), and what each later page holds (a list's rows by id where its rows carry one). The later pages are kept
# by this server per call, beside other calls' pages, until the same call again. Switch: CK3_REPLY_PAGES=off restores `fit` (and the listing) exactly.

import os as _os

PAGED = ("observe", "plan_war", "succession_plan")
#: (inspect title k_sweden 10,984 -> 7,987 bytes and cbs 12,519 -> 6,311, cut while those three were
#: paged): reads paged by their call key (reply_fit.call_key), the subject or the tool
PAGED_CALLS = ("inspect:title", "cbs")
#: The paged calls whose lists start on the first page with room left, not whole on a later page (observe,
#: plan_war and succession_plan keep their layout)
PAGE_SPLIT = PAGED_CALLS


def paged_key(tool: str, args: Optional[dict] = None) -> Optional[str]:
    """The key a reply is paged by (its PAGE_ORDER, its `more`), or None when the call is not paged."""
    if tool in PAGED:
        return tool
    k = call_key(tool, args)
    return k if k in PAGED_CALLS else None
PAGES_ENV = "CK3_REPLY_PAGES"
MORE_SUBJECT = "more"


def more_call() -> str:
    """The call that reads the next page, named as the client calls it (the host's tool-name prefix)."""
    return (_os.environ.get("CK3_TOOL_PREFIX") or "").strip() + 'inspect(subject="more")'

#: per tool, the blocks in the order a decision reads them; a block not named goes after these, in reply order
PAGE_ORDER = {
    "observe": ["goal", "standing", "de_jure", "wars", "war_brief", "war_stance_brief", "money", "liege",
                "succession", "since", "self", "news", "military", "council_brief", "council", "posture",
                "standing_policies", "evaluations"],
    "plan_war": ["empty_reason", "offensive_war_open", "committed_strength", "lifecycle", "precheck_rule",
                 "targets", "excluded"],
    # The summary first (a harness's de-jure metric reads summary.held off the first page; live, with the
    # counties split from page 1 the summary went to page 2 and the metric read unreadable), what the title needs and
    # the rules its rows name, then the counties (rows by key), then the rest
    "inspect:title": ["summary", "creation", "creation_rule", "next_call_templates", "counties", "duchies", "realms"],
    "cbs": ["target_state", "valid_options", "strength_verdict", "committed_strength", "offensive_war_open",
            "ducal_conquest", "defender_allies", "invalid_by_gate", "by_family"],
    "succession_plan": ["dynasty_warning", "line_risk", "outcome", "passes_intact", "at_war", "eligible_heir_count",
                        "succession_eta_years", "crown_titles", "primary_heir", "breaker", "prevent_split",
                        "death_estimate", "partition_forecast",
                        "levers", "per_title_map"],
}
#: The news rows in the order the goal reads them, when the news is laid out by its rows (after the kept ones):
#: a threat to the realm, what the realm gained or will lose, what the AI did or was stopped from doing on the goal's
#: chain, the purse; the rest (council, events, schemes, the map) after, in reply order
NEWS_FIRST = ("attackers_new", "wars_started", "war_declared_on", "factions_against_you", "faction_demands_answered",
              "titles_gained", "counties_left_realm", "asks_answered", "split_leaves_realm", "heir_split_ahead", "succession_turned_split", "claims_lost",
              "claims_gained", "wars_at_we_win", "allies_joined", "ai_vetoed", "idle", "purse", "fabrication",
              "letter_rule_raised", "claim_declined", "ai_acted")
#: (the turn's news said heir_split_ahead was on page 2, and the seat's next inspect title call
#: discarded those pages unread): the later pages of a reply are kept per call, beside the pages of other calls
KEPT_UNTIL = "the same call again, or the game date moving; other calls' pages are kept beside these"
#: per tool, {list: (fields a row's decision does not read first, the row field that names it)}: when a reply is
#: paged, these fields leave their rows for a `<list>_detail` block laid out after every other block (each entry
#: names its row), so more whole rows fit on the first page; nothing is dropped (plan_war: a row's cb_options were
#: 2.2 of its 4 KB at Munster, where 2 rows filled a page)
PAGE_DETAIL = {"plan_war": {"targets": (("cb_options",), "rank")}}    # rank: unique in a reply (names repeat)
#: the fields that name a row in the `more` block, first found wins
ROW_ID_FIELDS = ("id", "target_id", "character_id", "title_id", "key", "target_name", "law", "label", "name", "title")
#: the bytes the `more` block may take on the first page
MORE_RESERVE = 600
#: The kept pages, per call (store_key): {tool, pages, next, as_of, n, seq}; the newest read first by `more`
_STORES: dict = {}
_SEQ = [0]


def reset_pages() -> None:
    """Every kept page dropped (a test's fresh start)."""
    _STORES.clear()


def store_key(tool: str, args: Optional[dict] = None) -> str:
    """The call a reply's pages are kept by: its paged key, with observe's news-only read apart from observe."""
    a = args or {}
    return f"{tool}:news" if tool == "observe" and a.get("depth") == "news" else tool


def _unread(st: Optional[dict]) -> int:
    return max(0, len((st or {}).get("pages") or []) - int((st or {}).get("next") or 0))


def unread_pages() -> dict:
    """{call: its unread page count} for every call whose pages are kept and not all read."""
    return {k: _unread(v) for k, v in _STORES.items() if _unread(v)}


def pages_on() -> bool:
    """B3t's switch, read per call: on unless CK3_REPLY_PAGES is `off`."""
    return (_os.environ.get(PAGES_ENV) or "on").strip().lower() != "off"


def _row_id(row: Any) -> Any:
    if isinstance(row, dict):
        for f in ROW_ID_FIELDS:
            if row.get(f) not in (None, "", [], {}):
                return row[f]
    return None


def _ordered(res: dict, tool: str) -> list:
    """(key, value) of a result: scalars first (the summary), then PAGE_ORDER, then the rest in reply order."""
    order = PAGE_ORDER.get(tool, [])
    scal = [(k, v) for k, v in res.items() if not isinstance(v, (dict, list))]
    blocks = [(k, v) for k, v in res.items() if isinstance(v, (dict, list))]
    rank = {k: i for i, k in enumerate(order)}
    blocks.sort(key=lambda kv: (kv[0].endswith("_detail"), rank.get(kv[0], len(order))))
    return scal + blocks


class _Pager:
    """Lays (path, value) pieces into pages of at most `room` bytes of result each."""

    def __init__(self, room: int, first_room: int, split: bool = False):
        self.room, self.pages, self.later = room, [{}], {}
        self.rooms = [first_room]
        self.split = split                    # A list that fits no page begun goes on from the page it reached

    def _free(self, i: int) -> int:
        return self.rooms[i] - size(self.pages[i])

    @staticmethod
    def _put(page: dict, path: tuple, value: Any) -> None:
        cur = page
        for p in path[:-1]:
            cur = cur.setdefault(p, {})
        last = path[-1]
        if isinstance(value, list) and isinstance(cur.get(last), list):
            cur[last].extend(value)
        elif isinstance(value, dict) and isinstance(cur.get(last), dict):
            cur[last].update(value)           # A block whose `keep` parts were laid out first
        else:
            cur[last] = value

    def _fits(self, i: int, path: tuple, value: Any) -> bool:
        trial = json.loads(json.dumps(self.pages[i], default=str))
        self._put(trial, path, value)
        return size(trial) <= self.rooms[i]

    def _page_with_room(self, path: tuple, value: Any, start: int) -> Optional[int]:
        for i in range(start, len(self.pages)):
            if self._fits(i, path, value):
                return i
        self.pages.append({})
        self.rooms.append(self.room)
        i = len(self.pages) - 1
        return i if self._fits(i, path, value) else None

    def note(self, i: int, path: tuple, what: str) -> None:
        if i > 0:
            self.later.setdefault(i + 1, []).append(f"{'.'.join(map(str, path))}: {what}")

    def add(self, path: tuple, value: Any, start: int = 0) -> int:
        """Place a piece whole on the first page with room (from `start`), a too-big dict by its keys, a too-big list
        by its rows (in order, each whole). Returns the page it ended on."""
        if self.split and isinstance(value, list) and value:
            # (live 0867.01.01: inspect title k_sweden's 25 counties went whole to page 2, page 1 sent at 5,467
            # bytes): a list that fits no page already begun starts where the room is, its rows on in order
            i = next((j for j in range(start, len(self.pages)) if self._fits(j, path, value)), None)
        else:
            i = self._page_with_room(path, value, start)
        if i is not None:
            self._put(self.pages[i], path, value)
            self.note(i, path, f"{len(value)} rows" if isinstance(value, list) else "whole")
            return i
        if isinstance(value, dict) and value:
            last = start
            for k, v in value.items():
                last = self.add(path + (k,), v, start)
            return last
        if isinstance(value, list) and value:
            i, n, first_row = start, len(value), 0
            for j, row in enumerate(value):
                k = self._page_with_room(path, [row], i)
                if k is None:                         # one row over a whole page: the row as `fit` would send it
                    row = fit({"result": {"row": row}}, "", None, cap=self.room)["result"]
                    k = self._page_with_room(path, [row], i) or len(self.pages) - 1
                if k != i or j == 0:
                    if j > 0:
                        self._span(i, path, value, first_row, j, n)
                    i, first_row = k, j
                self._put(self.pages[k], path, [row])
            self._span(i, path, value, first_row, n, n)
            return i
        # a scalar over a whole page (never seen): sent as `fit` sends its largest value
        self.pages.append({})
        self.rooms.append(self.room)
        self._put(self.pages[-1], path, f"omitted: {size(value)} bytes")
        return len(self.pages) - 1

    def _span(self, i: int, path: tuple, rows: list, a: int, b: int, n: int) -> None:
        ids = [x for x in (_row_id(r) for r in rows[a:b]) if x is not None]
        what = f"rows {a + 1}-{b} of {n}" + (f" {json.dumps(ids, default=str)}" if ids else "")
        self.note(i, path, what)


def paginate(env: Any, tool: str, args: Optional[dict] = None, cap: int = CAP, store: bool = True) -> Any:
    """B3t: `env` itself when it is at most `cap` bytes; else its first page, the rest kept for `inspect subject=more`
    (module comment above). Pages are kept per call (store_key): a reply drops only the earlier pages of the
    same call (saying so when some were unread) and the pages of a game date gone by; `store` False (a read a harness
    makes for itself on the seat's server, never shown to the seat) leaves every kept page as it was."""
    if not isinstance(env, dict):
        return env
    skey = store_key(tool, args)
    as_of = (env.get("game") or {}).get("date") if isinstance(env.get("game"), dict) else None
    replaced = None
    if store:
        for k in [k for k, v in _STORES.items() if as_of is not None and v.get("as_of") not in (None, as_of)]:
            _STORES.pop(k)                                    # a game date gone by: its pages are of another game state
        gone = _unread(_STORES.pop(skey, None))
        if gone:
            replaced = {"call": skey, "unread_pages": gone, "why": "this reply of the same call is newer"}
    try:
        first = size(env)
    except (TypeError, ValueError):
        return env
    if first <= cap or not isinstance(env.get("result"), dict):
        if replaced is not None and isinstance(env.get("result"), dict):
            out = {**env, "result": {**env["result"], "pages_dropped": replaced}}
            if size(out) <= cap:
                return out
        return env
    res = dict(env["result"])
    if tool == "observe" and isinstance(res.get("news"), dict):
        rank = {n: i for i, n in enumerate(NEWS_FIRST)}             # A paged news lays the goal's rows first
        res["news"] = dict(sorted(res["news"].items(), key=lambda kv: rank.get(kv[0], len(rank))))
    for lst, (fields, name) in (PAGE_DETAIL.get(tool) or {}).items():
        rows = res.get(lst)
        if isinstance(rows, list) and any(isinstance(r, dict) and any(f in r for f in fields) for r in rows):
            res[lst] = [{k: v for k, v in r.items() if k not in fields} if isinstance(r, dict) else r for r in rows]
            res[lst + "_detail"] = [{name: r.get(name), **{f: r[f] for f in fields if f in r}}
                                    for r in rows if isinstance(r, dict) and any(f in r for f in fields)]
    frame = {k: v for k, v in env.items() if k != "result"}
    room = cap - size({**frame, "result": {}}) - 220          # a later page's own `more` line
    pager = _Pager(room, room - MORE_RESERVE, split=tool in PAGE_SPLIT)
    # The spec's `keep` parts go first, after the summary
    keep = keep_paths_of(SPECS.get(tool) or {}, res, tool)
    ordered = _ordered(res, tool)
    for k, v in ordered:
        if not isinstance(v, (dict, list)):
            pager.add((k,), v)
    placed = set()
    for kp in keep:
        parts = tuple(kp.split("."))
        pager.add(parts, _value_at(res, kp))
        placed.add(parts)

    def rest_of(path: tuple, v: Any) -> None:
        """A block holding kept parts: its other parts by their keys (a kept part deeper down: its block's other
        parts by theirs), each on the first page with room, so nothing kept is laid out twice."""
        inner = {pp[len(path):] for pp in placed if pp[:len(path)] == path and len(pp) > len(path)}
        if not inner or not isinstance(v, dict):
            pager.add(path, v)
            return
        for kk, vv in v.items():
            if (kk,) in inner:
                continue
            if any(len(i) > 1 and i[0] == kk for i in inner):
                rest_of(path + (kk,), vv)
            else:
                # (the turns after a death: the kept parts filled half of page one and the rest of the news,
                # every other row, went WHOLE to page 2, unread): the rest goes by its keys, the goal's first
                # (NEWS_FIRST), each on the first page with room
                pager.add(path + (kk,), vv)
    for k, v in ordered:
        if not isinstance(v, (dict, list)):
            continue
        rest_of((k,), v)
    pages = pager.pages
    n = len(pages)
    if n == 1:                                                # it fitted once laid out (never over by much)
        return {**frame, "result": pages[0]}
    more = {"page": f"1 of {n}", "on_later_pages": {str(p): w for p, w in sorted(pager.later.items())},
            "next": more_call(), "kept_until": KEPT_UNTIL}
    if store:
        others = unread_pages()
        if others:
            more["also_kept"] = {k: f"{v} unread page{'s' if v != 1 else ''}: {more_call()} reads them after these"
                                 for k, v in others.items()}
        if replaced is not None:
            more["pages_dropped"] = replaced
        _SEQ[0] += 1
        _STORES[skey] = {"tool": tool, "pages": [{**frame, "result": p} for p in pages[1:]], "next": 0,
                         "as_of": as_of, "n": n, "seq": _SEQ[0]}
    # The page line and the later pages' blocks lead page 1
    out = {**frame, "result": {"more": more, **pages[0]}}
    while size(out) > cap and more["on_later_pages"]:          # the ids list is what gives way, never a row
        p = max(more["on_later_pages"], key=lambda q: size(more["on_later_pages"][q]))
        more["on_later_pages"][p] = [w.split(" [", 1)[0] for w in more["on_later_pages"][p]]
        if size(out) > cap:
            more["on_later_pages"].pop(p)
    return out


def next_page() -> dict:
    """`inspect subject=more`: the next kept page of the newest paged reply with pages unread (then the next
    newest's), or a refusal naming why there is none."""
    live = sorted(((v.get("seq", 0), k) for k, v in _STORES.items() if _unread(v)), reverse=True)
    if not live:
        return {"ok": False, "tool": "inspect", "result": {},
                "blocked": {"gate": "no_more_pages",
                            "summary": "no page is kept: every observe, plan_war, succession_plan, inspect "
                                       "title or cbs reply of this game date was whole, or every page of it was read"},
                "warnings": [], "errors": [], "bridge_health": "ok"}
    key = live[0][1]
    st = _STORES[key]
    i = st["next"]
    st["next"] = i + 1
    page = json.loads(json.dumps(st["pages"][i], default=str))
    n = st.get("n", len(st["pages"]) + 1)
    rest = {k: v for k, v in unread_pages().items() if k != key}
    page["result"]["more"] = {"page": f"{i + 2} of {n}", "of": st.get("tool"), "as_of": st.get("as_of"),
                              **({"next": more_call()} if i + 1 < len(st["pages"]) or rest else {}),
                              **({"then": {k: f"{v} unread page{'s' if v != 1 else ''}" for k, v in rest.items()}}
                                 if rest else {})}
    return page
