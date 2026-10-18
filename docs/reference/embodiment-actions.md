# Embodiment Actions & Target Legibility

Ground-truth reference: **what you can do, the exact verb to do it, and how to
see who you can do it to.** Every interaction name below was enumerated live
from the running game (469 interactions). Names are exact — pass them verbatim
as `act(type="interaction", interaction_type="<name>", target_id=...)`.

**The legibility loop** — never blind-fire:
1. `discover(what="targets", action="<X>")` — who are the valid targets right now.
2. `discover(what="acceptance", interaction_type="<name>", target_id=T)` or
   `discover(what="interaction_validity", interaction="<short>", target_id=T)` —
   will this specific target accept / is it valid.
3. `act(type="interaction", interaction_type="<name>", target_id=T)`.

If you don't have a concrete `target_id`, you cannot act — find one via the
"see targets" column first.

---

## Conquest (the map-painting loop)

| Intent | How | See targets |
|--------|-----|-------------|
| Declare a specific war | `act(type="war", method="steered", target_id, casus_belli, cb_title_id, claimant_id)` — deterministic, named, no crash | `discover(what="targets", action="war")` → every (target, title, CB, pressed?) you can attack. Or `discover(what="war_targets")`. |
| Declare war (let AI pick) | `act(type="war", target_id, cb_index, cb_title_id)` | `discover(what="valid_cbs_strict", target_id=T)` |
| Press someone's claim | steered war with `claimant_id=<claimant>`, `casus_belli="claim_cb"` (claim must be PRESSED) | `discover(what="targets", action="claim", title_id=TITLE)` → all claimants, who is pressed/alive/ruler. Also `query(domain="claimants", title_id=TITLE)`. |
| **Fabricate a claim (NATIVE — the legitimate path)** | `act(type="council_task", task="task_fabricate_claim", target_title_id=<id>)` assigns the **Court Chaplain** (NOT the Chancellor — fabrication is the chaplain's task in this build) to fabricate, and it **actually applies + sticks** (native store wired 20260614). **A Court Chaplain must be SEATED** — if the chaplain seat is vacant the task has no executor and produces no claim (silent stall, L29; the bridge now returns `seat_vacant:true` + a warning when you assign to an empty seat). Check `query(domain="council")` `vacant_positions` first. The native AI (Council subsystem ON, the default) normally seats councillors itself within a few months — so usually you just **wait** for the chaplain seat to gain a `holder_id`, then assign the task. If a seat stays vacant, fill it with **`act(type="appoint", position_id=<seat id from query council>, character_id=<a courtier>)`** — ⚠ this is a bridge action, **NOT a game interaction**: do NOT call `discover(what="acceptance", interaction_type="appoint_councillor")` (no such interaction exists — it returns "not found"). The realm must be able to fabricate — feudal yes, administrative/tribal no (the call returns `assignable` + a `reason`/`reject_reason`). **#118 reasons:** `target_required` = you omitted `target_title_id` (this task targets a title — pass an `assignable:true` county); `target_rejected` = the game's target oracle culled that county (pick another `assignable` one, or your government/realm cannot fabricate — e.g. unreformed-Norse/tribal Halfdan-class realms can't, reform to feudal/clan first). To hold it against the AI re-picking, `control(ai_id)` + `steer(subsystems={CouncilTasks:false})` first. Fabrication eligibility is restrictive (often only certain counties are `assignable`) — that is the game's real constraint; play within it. **Do NOT grant claims via console `add_claim` / `execute_console_command` — that is cheating, and the console tool has been removed from the agent surface. Earn claims via fabrication, inheritance, and lawful CB wars.** | **`discover(what="fabricate_targets", actor_id=A)`** → counties that pass the game's real fabricate triggers for A; each entry has **`assignable`** (true ⇒ `set_council_task` will apply+stick) + `assignable_count`. Pick an `assignable:true` `title_id`. Confirm the eventual claim with `query(domain="claims")`. (NOTE: this list is currently title_id-sorted and can truncate nearby targets — a known legibility bug.) |
| See my own claims | — | `query(domain="claims", include_vassals=True)` |

> **Gate steered wars on `declarable`, NOT on `inner_valid_count`/`valid_count`.**
> Every war/CB/claimant discover surface (`valid_cbs_strict`, `war_targets`,
> `targets action=war|claim`, `claimants`) returns per-CB **`declarable`** plus a
> **`cull_reason`**, an aggregate **`declarable_count`**, and a `target_state`
> block; claimant surfaces also return **`backable_by_attacker`** /
> `not_backable_reason` / `in_attacker_court`. The engine's raw inner verdict
> (`inner_status`/`inner_valid_count`/`valid_count`) is **over-optimistic** — it
> passes `claim_cb` even against an UNPRESSED claim or a vassal/allied/truced/
> imprisoned target, because the CB-validity check tests neither pressed-ness nor
> target state. If you force a steered war on that raw verdict, the engine culls it
> to `war_count` 0. **Attempt a steered war only when `declarable` is true** (use
> `backable_by_attacker` for claim wars); a nonzero `inner_valid_count`/`valid_count`
> does NOT mean declarable. Otherwise read `cull_reason` (e.g. `claim_not_pressed`,
> `target_not_independent`, `target_imprisoned`, `target_in_alliance`,
> `target_in_truce`, `already_at_war_opposing`) and fix that precondition first.

## Prison & punishment (vassals & enemies)

| Intent | Interaction | See targets |
|--------|-------------|-------------|
| Imprison | `imprison_interaction` | `discover(what="targets", action="imprison")` → vassal pool + per-target grounds (has_imprisonment_reason). Single target: `discover(what="imprisonment", target_id=T)`. |
| Execute prisoner | `execute_prisoner_interaction` | `discover(what="imprisonment", target_id=T)` (execute_reason) |
| Release prisoner | `release_from_prison_interaction` | (your prisoners) |
| Ransom prisoner | `ransom_interaction` | (your prisoners) |
| Revoke a title | `revoke_title_interaction` | `discover(what="targets", action="revoke_title")` → vassal pool |

## Intrigue / schemes (launched as interactions)

| Intent | Interaction | Notes |
|--------|-------------|-------|
| Murder | `start_murder_interaction` | hostile scheme; pre-check acceptance not required (covert) |
| Seduce | `seduce_interaction` | romance scheme |
| Abduct | `start_abduct` | hostile scheme |
| Press a throne claim | `claim_throne_interaction` | become/insert heir |
| Sway (improve opinion) | `sway_interaction` | |
| Befriend | `befriend_interaction` | friendship scheme |
| Make / remove rival | `make_rival_interaction` / `remove_rival_interaction` | |

Check `query(domain="schemes")` for your active schemes;
`query(domain="opinion_breakdown", target_id=T)` for *why* a target likes/dislikes you.

## Diplomacy & dynasty

| Intent | Interaction | See targets |
|--------|-------------|-------------|
| Gift gold | `gift_interaction` | opinion lever; `query(domain="opinion", target_id=T)` |
| Arrange marriage | `arrange_marriage_interaction` | `query(domain="marriage_candidates")` → landed unmarried rulers IN diplomatic range of you, top-N by military, with age/distance (pass `require_in_range=false` to browse globally, `sort=` to rank by a skill). For courtier brides, find a strong ruler via `query(domain="characters", filter="independent")` then drill `query(domain="dynasty")`. Always confirm with `discover(what="acceptance", interaction_type="arrange_marriage_interaction", target_id=T)` — the browser does NOT apply faith/consanguinity/sex gates. |
| Divorce | `divorce_character_interaction` | |
| Negotiate alliance | `negotiate_alliance_interaction` | `query(domain="alliances")` for current allies + their real military strength. ⚠ **The alliance key is `negotiate_alliance_interaction` — NOT `form_alliance` / `form_alliance_interaction` / `ally_interaction`, which return `interaction_not_found` in this build.** Verified sibling keys (enumerated via the full interaction catalog): `perk_alliance_interaction` (perk-gated variant), `debug_alliance_interaction` (debug). |
| **Alliance HYGIENE — see which ally dragged you into war (#122)** | `query(domain="call_to_arms")` (read) | **THE army-stays-home lever.** Marriage alliances auto-drag the ruler into distant ally OFFENSIVE wars (a León/Iberia holy war tied up King Ragnarr's levies during his recovery trough — a major contributor to the succession-trough collapse). `query(domain="call_to_arms", character_id=R)` returns each active war the ruler was pulled into **as a called ally** (NOT a war primary): `called_by_ally`, `my_side`, **`is_offensive`** (true ⇒ attacking on a war you did not start = the "keep my army home" candidate), `opponent_primary` (the distant realm), `offensive_call_count`, and a `hint`. Cross-reference `query(domain="alliances")` to weigh the ally's worth vs the levy cost. Structurally validated against the 1097 core dump (6/6 hits). |
| **Break a liability alliance (#122)** | `act(type="break_alliance", target_id=<ally>)` | Withdraw from an alliance so it can no longer drag you into the ally's wars — the defensive-consolidation hygiene play (shed liability alliances before a succession trough). Routes through the legitimate engine interaction pipeline. ⚠ In THIS build `break_alliance` is **not exposed as a character interaction** (the 468-interaction catalog has no break/decline/leave-war key — breaking is a diplomacy-relation command, and AI call-to-arms is auto-resolved by the engine), so this currently returns `interaction_not_found`. The reliable, fully-working hygiene flow today is: **read `query(domain="call_to_arms")` → if `is_offensive` calls appear, do NOT renew/seek that alliance, and bias the ruler DEFENSIVE (steer boldness↓, DeclareWar off, raise MaA, bank gold) so the existing offensive call runs its course without you committing more; never sit in an ally-offensive war across a succession.** `act(type="decline_call", target_id=<ally>)` is the symmetric decline lever (same engine reality). |
| Grant title to vassal | `grant_titles_interaction` | your held titles + vassals (`query(domain="realm")`) |
| Grant independence | `grant_independence_interaction` | |

## Council & faith

| Intent | How | Notes |
|--------|-----|-------|
| **Seat a councillor (e.g. fill the Court Chaplain)** | **`act(type="appoint", position_id=<seat from query council>, character_id=<courtier>)`** | ⚠ **bridge action, NOT a game interaction** — do NOT `discover(acceptance, interaction_type="appoint_councillor")` (returns "not found"). Returns `applied:true` + `holder_after` on success. The native AI (Council subsystem ON) usually fills seats itself — prefer to **wait** unless a seat is persistently vacant. `replace=true` to swap an occupied seat. |
| **Change a councillor's task (incl. fabricate)** | **`act(type="council_task", task="task_fabricate_claim"\|"task_increase_control"\|…, target_title_id=<id>)`** | The proven native path (store wired 20260614). Returns `assignable` + `applied` + (if empty) `seat_vacant`+`warning`. The seat must be SEATED first (see above). `debug_change_council_task_interaction` is obsolete — do not use it. |
| Invite to council seat | `invite_to_council_position_interaction` | interaction; brings an outsider into court to then appoint |
| Force onto council | `force_onto_council` | interaction |
| **Convert your OWN faith** (e.g. Norse → Catholic) | **`act(type="faith", faith="<faith_key>")`** (e.g. `faith="catholic"`) | ⚠ **NOT `convert_to_religion_interaction`** — that interaction only opens a GUI faith-conversion view (no scripted effect), so it no-ops headless. The working lever is the engine setter `SetFaithOnCharacter` (`set_character_faith`): a real conversion with `on_faith_conversion` + recalc side effects. Takes a faith KEY (or `faith_id`). Voluntary self-conversion is auto-accept; honor the in-game gates yourself (must be landed, must differ from current faith). Must be UNPAUSED. Returns `previous_faith`/`current_faith` on success. ★ Use this to convert a wrong-faith kingdom (e.g. k_danelaw Norse → the local Christian faith) to STOP being holy-warred. |
| Add a doctrine/tenet to your faith | `act(type="faith", doctrine="<doctrine_key>")` | `set_doctrine` (ADD-only). Distinct from conversion — pass `doctrine=` not `faith=`. |
| **Convert your OWN culture** (e.g. Norse → English) | **`act(type="culture", culture="<culture_key>")`** (e.g. `culture="english"` or `culture="anglo_saxon"`) | ⚠ There is NO in-game interaction/decision for a ruler to swap its own culture wholesale headlessly. The working lever is the engine setter `SetCultureOnCharacter` (`set_character_culture`, ck-#130): a real conversion with full culture/heritage/innovation recalc, NOT a memory poke. Takes a culture KEY (or `culture_id`). Voluntary self-conversion is cost-free; must be UNPAUSED. Returns `previous_culture`/`current_culture` on success. ★★ **THE e_britannia UNLOCK** — `e_britannia.can_create` is culture-gated on `heritage_west_germanic` (english/anglo_saxon) / goidelic / brythonic; a NORSE ruler can form it by NEITHER path, so convert to `english`/`anglo_saxon` (after holding English land, before a succession so the heir inherits it) to make the create_title/reclaim path legal. |

## Realm law & high-stakes decisions (set expectations)

Two refusals here are **working-as-designed legitimacy gates, NOT bugs or
blockers** — do not retry them, do not route around them with the console, and
do not narrate them as "the tool is broken."

- **Crown-authority / succession-law changes refuse with
  `reason:"high_stakes_law_uncosted"`.** `act(type="law", law_name="crown_authority_2")`
  (and any `crown_authority*` / succession law via `set_title_law`) is **intentionally
  blocked.** The bridge's uncosted law path would skip the real prestige cost, the
  ~10-year cooldown, the powerful-vassals vote, and the vassal-opinion (illegitimate)
  penalty — applying it that way is a cheat. So it is refused unless a caller passes
  `allow_uncosted=true` (harness-only setup, never the agent). **Crown-authority and
  succession-law progression is therefore the NATIVE AI's job:** steer toward it with
  personality weights (dominance↑, rationality↑) and let the realm's own lawmaking move
  it over reigns. Treat this as a tempo lever, not a button — do not keep firing the
  law verb expecting it to take. (Source: `reject_uncosted_high_stakes_law`,
  `bridge/src/action_handlers.cpp`; write-action legitimacy audit
  `re/findings/re_write_action_legitimacy_audit.md`.)

- **A rejected decision sometimes returns a terse "cannot take decision" with no
  per-trigger reason** (seen with custom/scripted decisions like
  `merge_aquitaine_decision`). This is an **engine-side** terseness: the engine's
  decision evaluator returns a single generic string when an `is_shown`/`is_valid`
  trigger fails, and the bridge surfaces it verbatim (`engine_result` / `reason`). It is
  **not** a bridge truncation bug, and there is no richer reason-string today. When you
  see it, **do not retry blindly** — use `discover(what="decisions_annotated",
  decisions=["<name>"])` (game UNPAUSED) for the per-decision shown/valid/cost_ok
  breakdown + the failing requirement loc-keys, or accept that the decision's
  preconditions aren't met yet and move on.

- **`found_empire_decision` (and other empire-foundation decisions) are COST-gated,
  NOT AI-passivity-gated.** They carry a large prestige/piety resource cost. The
  ruler can be otherwise eligible (`shown`/`valid`/`cost_ok` all true) yet `act()`
  returns `engine_result:"cannot afford decision"` for lack of funds. `discover(what=
  "valid_decisions"/"decisions_annotated", decisions=["found_empire_decision"])` now
  honors this: it emits `afford_ok` (current gold/piety/prestige vs the decision's
  `cost{}`) and folds it into `available`, so the `valid_decisions` shortlist only
  lists a decision once it is truly affordable, and `failures` carries
  `"cannot_afford_decision"`. **Read `available`/`afford_ok` before recommending an
  empire as "ready" — do NOT misdiagnose an unaffordable empire as AI passivity** (the
  Ghaznavid realm fractured on partition because the empire was never banked-for, not
  because the AI refused). BANK prestige/piety first.

(Generic note: an `unknown_action_type` error from `act()` just means the `type`
string isn't one of the supported verbs — re-read the verb table at the top of this
doc and retry with a valid `type`; this is self-correcting, not a capability gap.)

---

## What is NOT reachable yet (honest gaps)

These have **no working verb** today — do not pretend they do. Use
`execute_console_command` as an escape hatch where a console command exists.

- ~~Marriage-candidate browser~~ — **shipped** as `query(domain="marriage_candidates")`
  (landed unmarried rulers in range). Faith/consanguinity/sex gates still deferred
  to `discover(what="acceptance")`. (`query_marriage_observations` remains passive
  telemetry, unrelated.)
- ~~Council task assignment (clean path)~~ — **shipped** as
  `act(type="council_task", …)` (native store, 20260614) and councillor seating as
  `act(type="appoint", …)`. Remaining gap: **no court-roster / council-candidate
  read** — to manually appoint you must already know a courtier's `character_id`
  (the native AI seats councillors on its own, so this is usually unnecessary).
- **Building construction** — no interaction; needs a dedicated handler. Task #26.
- **Lifestyle focus / perks** — no interaction; needs SET_FOCUS handler. Task #26.

When you hit one of these, say so plainly in your narration and route around it
(steer the native AI via personality weights, or use the console escape hatch),
rather than firing an interaction that doesn't exist.
