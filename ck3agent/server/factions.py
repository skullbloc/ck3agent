"""Faction management from both seats (cleanup 2026.11, F16).

The vassal's seat:
  do(action_class="factions_to_join")   every faction type the vassal may create
      against its liege, every faction aimed at that liege it may join, the ones it
      is in (may it leave), and for one it leads whether it may press the demand now;
      each row carries the game's own verdict and, when refused, the game's reason.
  do(intent="faction", args={action: "create", faction_type})
  do(intent="faction", args={action: "join" | "leave" | "press_demand", faction_id})

The liege's seat:
  inspect(subject="factions")           every faction aimed at the ruler: type,
      leader, members (names, soldiers), discontent against the threshold, power,
      whether its AI leader may press the demand and his chance each month, the
      war the faction fights if refused, the ruler's standing answer to a demand
      and the answers given; and per leader the levers a player has: a gift,
      imprisonment (judged by the game's own send check, with the leader's answer
      and score: redesign.imprison_rows), and the council seats he already holds.
  do(intent="faction", args={action: "respond_demand", response: "accept" | "refuse" | "ai"})
      sets the ruler's standing answer to a faction's ultimatum. A demand arrives as
      an event the game answers for an AI-run ruler; the bridge answers it with the
      named option for this ruler (bridge factions.cpp, AI_PICK_EVENT_OPTION).

Every write is read back through a second read before it is reported applied:
create, join and leave through the member list the game keeps (query_factions on
the liege the faction is aimed at), press_demand through the leader's own faction
read on a later tick (query_joinable_factions), where the game must now refuse a
second press. A second read that cannot see the faction reports write_unconfirmed.
"""
from __future__ import annotations

from typing import Any

FACTION_ACTIONS: tuple = ("create", "join", "leave", "press_demand", "respond_demand")
DEMAND_RESPONSES: tuple = ("accept", "refuse", "ai")

#: The interaction that appeases a faction leader: the game's own interaction key,
#: sent through do(intent="interact"); it is not a faction action and the bridge
#: has no faction-specific path for it. Imprisonment is redesign.imprison_rows, the
#: same rows the imprison_targets enumeration gives.
GIFT_INTERACTION = "gift_interaction"


def _R():
    import redesign  # noqa: PLC0415 — redesign imports this module
    return redesign


def _int(v) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


#: Every refusal code factions.cpp can emit that is not the game's own sentence,
#: said in plain words (test_factions enumerates the codes from the C++ source and
#: requires one here). `{leader}` is the leader the bridge names.
REFUSAL_SENTENCES: dict = {
    # the bridge's own pre-checks, made before the game is asked
    "not_the_faction_leader": "you are not this faction's leader; the leader is {leader}, "
                              "and only the leader presses its demand",
    "not_a_member": "you are not a member of this faction, so there is nothing to leave",
    "actor_has_no_liege": "you have no liege, so there is no one to form a faction against",
    "already_a_member": "you are already a member of this faction",
    "faction_type_not_found": "the game has no faction type by that name; "
                              "do(action_class='factions_to_join') lists the ones you may create",
    "actor_not_alive": "this character is dead",
    "actor_not_found": "the game has no character with that id",
    "faction_not_found": "there is no faction with that id now (factions dissolve when their "
                         "members leave or their demand is settled)",
    "faction_id_required": "this action needs args.faction_id",
    "faction_type_required": "create needs args.faction_type",
    "actor_id_required": "no acting character was given",
    "character_id_required": "no character was given",
    "unknown_faction_action": "the faction action must be create, join, leave or press_demand",
    "response_must_be_accept_refuse_or_ai": "the answer must be accept, refuse or ai",
    "not_a_controlled_character": "you do not hold this character; session(action='add') first",
    "rng_scope_latched": "faction actions are off for the rest of this game session: the bridge "
                         "stopped using the game's random-number scope after a safety check failed",
    # the bridge could not complete the game's call
    "faction_call_crashed": "the bridge's call into the game's faction code faulted and was stopped",
    "engine_call_faulted": "the game's check for this row faulted and was stopped, so the row has no verdict",
    "faction_query_exception": "the bridge failed while building the faction read; read again",
    "faction_returned_null_sentinel": "the game made no faction (it returned its empty faction)",
    "demand_hook_not_installed": "the bridge could not hook the game's answer to demands, so it "
                                 "cannot set one",
    "intent_timeout_no_result": "the game did not run the action within 5 seconds; it may still run",
    "faction_query_busy": "another faction read was running; read again",
    "timeout_waiting_for_game_thread": "the game's thread did not take the read within its wait; "
                                       "read again",
    # the game's call ran and its readback disagrees
    "still_a_member_after_leave": "the game's leave ran and you are still a member",
    "demand_still_pressable_after_press": "the press ran and the game would still let it be pressed, "
                                          "so the demand was not sent",
    "actor_not_member_after_call": "the game's call ran and you are not a member of the faction",
}
#: The bridge's own pre-checks: gate faction_precondition (never the game's rule).
PRECHECK_CODES: tuple = ("not_the_faction_leader", "not_a_member", "actor_has_no_liege")
#: The game's own refusals: the bridge asked the game's check and it said no. Their
#: summary is the game's sentence (gate faction_refused).
ENGINE_REFUSALS: tuple = ("engine_refused_create", "engine_refused_join",
                          "engine_refused_leave", "engine_refused_demand")


def _who(row: dict):
    """'Name (id)' of the leader a bridge row names, or whichever of the two it has."""
    name, cid = row.get("leader_name"), row.get("leader_id")
    return f"{name} ({cid})" if name and cid else (name or cid)


def _refusal(code: str, engine_reason: str = "", leader=None, drain=None) -> dict:
    """The blocked block for a faction refusal. A bridge pre-check is said in plain
    words (faction_precondition); the game's refusal carries the game's sentence
    (faction_refused) in `summary` and, as on every write (redesign.write_blocked),
    in `detail`; a drain refusal carries the drain's words in `detail`; any other
    code keeps the shared table's gate, said in plain words (REFUSAL_SENTENCES)."""
    R = _R()
    said = REFUSAL_SENTENCES.get(code, "").format(leader=leader or "unknown")
    if code in PRECHECK_CODES:
        return R.write_blocked(code, gate="faction_precondition", summary=said)
    if code in ENGINE_REFUSALS:
        # the reply's engine_reason is the game's words (write_blocked's detail)
        return R.write_blocked(code, gate="faction_refused",
                               summary=engine_reason or R.NO_GAME_DETAIL,
                               raw={"engine_reason": engine_reason})
    return R.write_blocked(code, summary=engine_reason or said or code,
                           drain=drain if isinstance(drain, dict) else None)


# ---------------------------------------------------------------------------
# the vassal's seat: what may I do?
# ---------------------------------------------------------------------------

async def options(gw, character_id: int) -> dict:
    """query_joinable_factions: the game's verdicts, computed on the game thread."""
    res = await gw.read("query_joinable_factions", {"character_id": character_id})
    return res if isinstance(res, dict) else {}


def option_rows(res: dict) -> tuple[list, list, list]:
    """(available, unavailable, warnings) do_call rows from query_joinable_factions."""
    R = _R()
    available: list = []
    unavailable: list = []
    warnings: list = []
    if res.get("error"):
        err = str(res.get("error"))
        warnings.append({"code": "faction_options_unread", "raw_reason": err,
                         "detail": REFUSAL_SENTENCES.get(err, err)})
        return available, unavailable, warnings
    if res.get("independent"):
        warnings.append({"code": "no_liege", "detail": res.get("note") or
                         "an independent ruler has no liege to form a faction against"})
        return available, unavailable, warnings
    liege = res.get("liege_name") or res.get("liege_id")

    def place(row: dict, verdict, code: str, reason: str) -> None:
        if verdict is True:
            row["blocked"] = None
            available.append(row)
        else:
            row["blocked"] = (_refusal(code, reason) if verdict is False
                              else R.why_blocked("faction_bridge_fault",
                                                 summary="the game's check faulted; it was contained",
                                                 raw_reason="faction_call_crashed"))
            unavailable.append(row)

    for t in res.get("faction_types") or []:
        if not isinstance(t, dict) or not t.get("faction_type"):
            continue
        ft = t["faction_type"]
        row = {"label": f"create {ft} against {liege}",
               "do_call": {"intent": "faction", "args": {"action": "create", "faction_type": ft}},
               "war_if_refused": t.get("war_if_refused")}
        if t.get("existing_faction_id"):
            row["existing_faction_id"] = t["existing_faction_id"]
        place(row, t.get("can_create"), "engine_refused_create", t.get("reason") or "")

    for f in res.get("factions") or []:
        if not isinstance(f, dict) or not f.get("faction_id"):
            continue
        fid, ft = f["faction_id"], f.get("faction_type")
        who = f.get("leader_name") or f.get("leader_id")
        base = {"faction_id": fid, "faction_type": ft, "leader": who,
                "member_count": f.get("member_count")}
        if f.get("is_member") is True:
            lv = f.get("leave") or {}
            place({**base, "label": f"leave {ft} ({fid})",
                   "do_call": {"intent": "faction", "args": {"action": "leave", "faction_id": fid}}},
                  lv.get("can_leave"), "engine_refused_leave", lv.get("reason") or "")
            dm = f.get("demand") or {}
            row = {**base, "label": f"press the demand of {ft} ({fid})",
                   "do_call": {"intent": "faction", "args": {"action": "press_demand", "faction_id": fid}},
                   "demand": {k: v for k, v in dm.items() if k not in ("can_press_now", "reason")}}
            if not f.get("is_leader"):
                row["blocked"] = _refusal("not_the_faction_leader", leader=_who(f))
                unavailable.append(row)
            else:
                place(row, dm.get("can_press_now"), "engine_refused_demand", dm.get("reason") or "")
        elif f.get("is_member") is False:
            jn = f.get("join") or {}
            place({**base, "label": f"join {ft} ({fid}) led by {who}",
                   "do_call": {"intent": "faction", "args": {"action": "join", "faction_id": fid}}},
                  jn.get("can_join"), "engine_refused_join", jn.get("reason") or "")
    return available, unavailable, warnings


# ---------------------------------------------------------------------------
# the liege's seat: who threatens me, and what can I do about them?
# ---------------------------------------------------------------------------

async def liege_view(gw, character_id: int, actor_id: int) -> dict:
    """The factions aimed at `character_id`, with their members, threat, demand
    state and the levers against each leader."""
    fac = await gw.read("query_factions", {"character_id": character_id})
    fac = fac if isinstance(fac, dict) else {}
    thr = await gw.read("query_faction_threat", {"character_id": character_id})
    thr = thr if isinstance(thr, dict) else {}
    threat_by_id = {t.get("faction_id"): t for t in thr.get("factions") or [] if isinstance(t, dict)}
    council = await gw.read("query_council", {"character_id": character_id})
    seats = [s for s in (council.get("council") or []) if isinstance(s, dict)] \
        if isinstance(council, dict) else []

    rows = []
    for f in fac.get("factions") or []:
        if not isinstance(f, dict):
            continue
        fid = f.get("faction_id")
        t = threat_by_id.get(fid) or {}
        row = {
            "faction_id": fid,
            "type": f.get("type"),
            "is_county_faction": bool(f.get("is_county_faction")),
            "leader_id": f.get("leader_character_id"),
            "leader_name": f.get("leader_name"),
            "members": f.get("member_rows") or [],
            "member_count": f.get("member_count"),
            "county_member_count": f.get("county_member_count"),
            "discontent": t.get("discontent", f.get("discontent_pct")),
            "discontent_threshold": thr.get("discontent_threshold_pct", fac.get("discontent_threshold_pct")),
            "power_pct_of_liege": t.get("power_pct_of_liege"),
            "threat_level": t.get("threat_level"),
            **_ultimatum(t),
            # The bridge's one reading (faction_war.h): the faction's war as the
            # game's own press check resolves it; war_id is the war's full id, the
            # one the war reads and the war intents take.
            "at_war": f.get("at_war") is True,
            "war_id": f.get("war_id"),
        }
        rows.append(row)
    # The levers against each leader; imprisonment is judged by the game for all
    # of them in one check (redesign.imprison_rows, one game tick).
    leads = [_int(r["leader_id"]) for r in rows if _int(r["leader_id"])]
    jail = dict(zip(leads, await _R().imprison_rows(gw, actor_id, leads))) if leads else {}
    for row in rows:
        lead = _int(row["leader_id"])
        if lead:
            row["levers"] = _leader_levers(lead, seats, jail[lead])
    return {
        "character_id": fac.get("character_id", character_id),
        "factions": rows,
        "faction_count": fac.get("faction_count", len(rows)),
        "critical_threats": thr.get("critical_threats"),
        "high_threats": thr.get("high_threats"),
        "demand_response": fac.get("demand_response"),
        "demands_answered": fac.get("demands_answered") or [],
        "respond_with": {"intent": "faction",
                         "args": {"action": "respond_demand", "response": "accept | refuse | ai"}},
    }


#: The bridge's demand classification (factions.cpp classify_faction_demand, the
#: one definition), relayed as the bridge gives it; nothing here re-derives it.
DEMAND_STATE_KEYS: tuple = ("ultimatum", "ai_press_chance_pct", "demand_cooldown_until")


def _ultimatum(threat_row: dict) -> dict:
    """The demand as the bridge classified it: `ultimatum` (ai_may_press with the
    AI leader's chance at each monthly faction tick, or none) and the cooldown
    date while one runs. No threat row for the faction is no reading: no keys."""
    return {k: threat_row[k] for k in DEMAND_STATE_KEYS if k in threat_row}


def _leader_levers(leader_id: int, seats: list, imprison: dict) -> dict:
    """A gift and imprisonment: the do_calls a player has against a faction
    leader, imprisonment as the game judges it (redesign.imprison_rows: offered only
    when the game would take the send and the leader's answer is not "no", with that
    answer and his score); and the council seats the leader already holds."""
    # The council is read, not offered as a lever. The bridge's appoint is the
    # game's seat swap, which trades the seats of two characters who BOTH sit on
    # the council already (re_resolution_appoint.md, precondition FUN_05c851f0):
    # it cannot seat a leader who has no seat, and moving a sitting leader to
    # another seat gives him nothing; the ruler's AI also moves him back to the
    # seat his skills suit (runs l10-p5-r2, l10-p6-r2, w-l10-factions-1: back
    # within 45 days).
    holds = [s.get("position_key") for s in seats if s.get("holder_id") == leader_id]
    return {
        "on_council": holds,
        "gift": {"do_call": {"intent": "interact",
                             "args": {"interaction": GIFT_INTERACTION, "target_id": leader_id}}},
        "imprison": imprison,
    }


async def inspect_factions(gw, character_id: int) -> dict:
    R = _R()
    actor, err = await R._resolve_actor(gw, character_id, "inspect")
    if err:
        return err
    cid = _int((actor or {}).get("id")) or character_id
    view = await liege_view(gw, cid, cid)
    return R.envelope("inspect", result={"subject": "factions", **view}, actor=actor,
                      game=gw.game(), bridge_health=gw.health)


# ---------------------------------------------------------------------------
# do(intent="faction")
# ---------------------------------------------------------------------------

async def _members_of(gw, liege_id: int, faction_id: int):
    """(read_ok, member ids, faction row) from query_factions on the liege the
    faction is aimed at (the game's member array). read_ok is False when the read
    failed or returned no faction list; then nothing can be said about the faction.
    With read_ok True, members is None when the liege has no such faction."""
    if not liege_id:
        return False, None, None
    fac = await gw.read("query_factions", {"character_id": liege_id})
    if not isinstance(fac, dict) or fac.get("error") or not isinstance(fac.get("factions"), list):
        return False, None, None
    for f in fac["factions"]:
        if isinstance(f, dict) and _int(f.get("faction_id")) == faction_id:
            return True, {_int(m) for m in f.get("members") or []}, f
    return True, None, None


async def _demand_row(gw, actor_id: int, faction_id: int):
    """(read_ok, demand block) for a faction the actor is in, from
    query_joinable_factions: a read the game computes on a later tick than the
    press, through the engine's own press check and the faction's fields."""
    res = await options(gw, actor_id)
    if not res or res.get("error") or not isinstance(res.get("factions"), list):
        return False, None
    for f in res["factions"]:
        if isinstance(f, dict) and _int(f.get("faction_id")) == faction_id and f.get("is_member") is True:
            dm = f.get("demand")
            return True, dm if isinstance(dm, dict) else None
    return True, None


async def do_faction(gw, args: dict, character_id: int, actor: dict) -> dict:
    R = _R()
    action = str(args.get("action") or "").strip()
    actor_id = _int(character_id) or _int((actor or {}).get("id"))

    def rejected(blocked: dict, result: dict | None = None) -> dict:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                          result=result, blocked=R.concrete_next(blocked, "faction"),
                          bridge_health=gw.health)

    if action not in FACTION_ACTIONS:
        return rejected(R.why_blocked(
            "missing_required_argument" if not action else "invalid_argument",
            summary=f"args.action must be one of {', '.join(FACTION_ACTIONS)}",
            fix={"rejected_fields": ["action"], "valid": list(FACTION_ACTIONS)}))
    if not actor_id:
        return rejected(R.why_blocked("actor_unresolved", summary="no acting character"))

    if action == "respond_demand":
        return await _respond_demand(gw, args, actor_id, actor)

    params: dict[str, Any] = {"actor_id": actor_id, "faction_action": action}
    faction_id = _int(args.get("faction_id"))
    if action == "create":
        ft = str(args.get("faction_type") or "").strip()
        if not ft:
            return rejected(R.why_blocked("missing_required_argument",
                                          summary="create needs args.faction_type",
                                          fix={"rejected_fields": ["faction_type"]}))
        params["faction_type"] = ft
        if _int(args.get("liege_id")):
            params["target_id"] = _int(args.get("liege_id"))
    else:
        if not faction_id:
            return rejected(R.why_blocked("missing_required_argument",
                                          summary=f"{action} needs args.faction_id",
                                          fix={"rejected_fields": ["faction_id"]}))
        params["faction_id"] = faction_id

    submitted_on = gw.game().get("date")
    raw = await gw.write("faction_action", params)
    raw = raw if isinstance(raw, dict) else {}
    res = raw.get("result") if isinstance(raw.get("result"), dict) else {}
    reason = str(raw.get("reason") or raw.get("error") or "")
    if raw.get("success") is not True:
        if reason == "experimental_write_disabled":
            return rejected(R.experimental_gate_blocked(raw, "faction_action"))
        if reason in ("intent_timeout_no_result", "") and not res:
            # The handler's own wait for the drain ran out (L13 amendment 3, stage
            # 1): settle_write reads the verdict back. A refusal is the game's. A
            # faction write is an "on_execute" lever (redesign.LEVERS ("faction",
            # "*")): a write that ran is read back in the same call, and here it
            # cannot be (the faction and liege the second read needs are in the
            # reply the handler did not get to send), so it is pending readback
            # (ok:true, result.sent), never applied, never rejected, never a
            # days-wait. A verdict not read is pending on the next pass.
            lever = R.lever_of("faction", sub=action)[1]
            conf = await R.settle_write(None, gw.read, raw, None,
                                        game_date_at_submit=submitted_on)
            drain = conf["drain"]
            if conf.get("refused"):
                return rejected(_refusal(conf["refused"], "", None, drain=drain),
                                result={"intent": "faction", "action": action, "raw": raw,
                                        "drain": drain["row"]})
            if conf["sent"] and lever["shows"] == "on_execute":
                # it ran, and the reply naming the faction to read back never came:
                # pending readback, ok:true, never rejected (FF5 ruling)
                return R.envelope("do", verdict="pending", actor=actor, game=gw.game(),
                                  pending=R.write_pending(
                                      "readback", game_date_at_submit=submitted_on,
                                      intent_id=conf.get("intent_id") or "",
                                      raw_reason="write_unconfirmed",
                                      expected_change=f"faction {action}",
                                      why_pending=(f"the drain ran the faction {action} and the "
                                                   "bridge's reply, which names the faction it "
                                                   "changed, never came, so it could not be read back"),
                                      verify_with="do(action_class='factions_to_join') before "
                                                  "sending it again"),
                                  result={"intent": "faction", "action": action, "raw": raw,
                                          "sent": True, "drain": drain["row"]},
                                  bridge_health=gw.health)
            return R.envelope("do", verdict="pending", actor=actor, game=gw.game(),
                              pending=R.conf_pending(
                                  conf, expected_change=f"faction {action}",
                                  why_pending=(f"the bridge's wait for the drain ran out; the "
                                               f"drain's verdict reads {drain['status']!r}"),
                                  verify_with="do(action_class='factions_to_join')"),
                              result={"intent": "faction", "raw": raw, "sent": conf["sent"],
                                      "drain": drain["row"]},
                              bridge_health=gw.health)
        return rejected(_refusal(reason or "unmapped_refusal", str(res.get("engine_reason") or ""), _who(res)),
                        result={"intent": "faction", "action": action, "raw": raw})

    # The bridge checked the outcome inside the same call; a second read, outside
    # it, decides the verdict. Its answer is one of: shows it (applied), shows the
    # opposite (rejected), or could not look (unconfirmed, never applied).
    faction_id = _int(res.get("faction_id")) or faction_id
    verdict, readback = await _second_read(gw, action, res, actor_id, faction_id)
    result = {"intent": "faction", "action": action, "faction_id": faction_id,
              "bridge": res, "readback": readback}
    if verdict == "applied":
        if action == "press_demand":
            target = readback.get("liege_id") or "the liege"
            result["next"] = (f"the demand is an event {target} answers within days: read the answer on "
                              f"inspect(subject='factions', character_id={target}).demands_answered, or "
                              f"do(action_class='factions_to_join') demand.demands_answered")
        return R.envelope("do", verdict="applied", actor=actor, game=gw.game(), result=result,
                          bridge_health=gw.health)
    if verdict == "unconfirmed":
        # The bridge reported it done and the second read could not be made: the
        # write ran, so it is pending readback, ok:true, never rejected (FF5
        # ruling: no write the drain ran is rejected for a reading the server
        # could not make).
        return R.envelope("do", verdict="pending", actor=actor, game=gw.game(),
                          result=dict(result, sent=True),
                          pending=R.write_pending(
                              "readback", game_date_at_submit=submitted_on,
                              intent_id=R.intent_id_of(raw), raw_reason="write_unconfirmed",
                              expected_change=f"faction {action}",
                              why_pending=(f"the bridge reported {action} done and the second read "
                                           f"could not see the faction: {readback.get('unseen')}"),
                              verify_with="do(action_class='factions_to_join') before sending it again"),
                          bridge_health=gw.health)
    return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                      result=result,
                      blocked=R.write_blocked(gate="faction_write_not_applied",
                                            summary=f"the bridge reported {action} done and the second "
                                                    f"read shows it did not take effect",
                                            raw_reason="readback_disagrees"),
                      bridge_health=gw.health)


async def _second_read(gw, action: str, res: dict, actor_id: int, faction_id: int):
    """('applied' | 'rejected' | 'unconfirmed', readback) for a faction write the
    bridge reported done.

    create / join / leave read the member array on the liege the faction is aimed
    at (the bridge names it: target_id, or liege_id for create; never the actor's
    own liege, which is not always the target: run l10-dev3). press_demand reads
    the faction's demand from the leader's side on a later tick
    (query_joinable_factions), where the engine's press check must now refuse."""
    if action == "press_demand":
        rb: dict[str, Any] = {"via": "query_joinable_factions", "faction_id": faction_id,
                              "liege_id": _int(res.get("target_id"))}
        read_ok, dm = await _demand_row(gw, actor_id, faction_id)
        if not read_ok or dm is None:
            rb["unseen"] = ("the leader's faction read failed" if not read_ok
                            else "the leader is no longer listed in that faction")
            return "unconfirmed", rb
        rb["demand"] = {k: dm.get(k) for k in ("can_press_now", "reason", "demand_cooldown_until",
                                               "ultimatum", "ai_press_chance_pct", "at_war", "war_id",
                                               "war_if_refused") if k in dm}
        if dm.get("can_press_now") is False:
            return "applied", rb
        if dm.get("can_press_now") is True:
            return "rejected", rb
        rb["unseen"] = "the game's press check faulted on the second read"
        return "unconfirmed", rb

    liege_id = _int(res.get("target_id")) or (_int(res.get("liege_id")) if action == "create" else 0)
    rb = {"via": "query_factions", "liege_id": liege_id, "faction_id": faction_id}
    if not liege_id:
        rb["unseen"] = "the bridge did not name the character the faction is aimed at"
        return "unconfirmed", rb
    read_ok, members, frow = await _members_of(gw, liege_id, faction_id)
    if not read_ok:
        rb["unseen"] = f"query_factions on {liege_id} failed"
        return "unconfirmed", rb
    if action in ("create", "join"):
        rb["member"] = members is not None and actor_id in members
        return ("applied" if rb["member"] else "rejected"), rb
    # leave
    if members is not None:
        rb["member"] = actor_id in members
        return ("rejected" if rb["member"] else "applied"), rb
    # The faction is gone from the liege's list. That agrees with the leave only
    # when the engine's own call said it disbanded the faction.
    rb["faction_disbanded"] = True
    if res.get("faction_disbanded") is True:
        rb["member"] = False
        return "applied", rb
    rb["unseen"] = (f"faction {faction_id} is not on {liege_id}'s list and the leave did not say "
                    f"it disbanded")
    return "unconfirmed", rb


async def _respond_demand(gw, args: dict, actor_id: int, actor: dict) -> dict:
    R = _R()
    resp = str(args.get("response") or "").strip()
    if resp not in DEMAND_RESPONSES:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                          blocked=R.why_blocked(
                              "missing_required_argument" if not resp else "invalid_argument",
                              summary=f"respond_demand needs args.response in {', '.join(DEMAND_RESPONSES)}",
                              fix={"rejected_fields": ["response"], "valid": list(DEMAND_RESPONSES)}),
                          bridge_health=gw.health)
    raw = await gw.write("set_faction_demand_response", {"character_id": actor_id, "response": resp})
    raw = raw if isinstance(raw, dict) else {}
    if raw.get("success") is not True:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                          result={"intent": "faction", "action": "respond_demand", "raw": raw},
                          blocked=_refusal(str(raw.get("reason") or "unmapped_refusal")),
                          bridge_health=gw.health)
    fac = await gw.read("query_factions", {"character_id": actor_id})
    stored = (fac or {}).get("demand_response") if isinstance(fac, dict) else None
    result = {"intent": "faction", "action": "respond_demand", "response": resp,
              "readback": {"via": "query_factions", "demand_response": stored},
              "applies_to": "every faction demand made of this ruler from now on, until changed"}
    if stored != resp:
        return R.envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                          result=result,
                          blocked=R.write_blocked(gate="faction_write_not_applied",
                                                summary=f"set {resp}, the bridge reads back {stored!r}",
                                                raw_reason="readback_disagrees"),
                          bridge_health=gw.health)
    return R.envelope("do", verdict="applied", actor=actor, game=gw.game(), result=result,
                      bridge_health=gw.health)
