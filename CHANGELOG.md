# Changelog

All notable changes to ck3agent. Versions are calendar-based
(`YYYY.MM.PATCH`); the git tag is the version with a `v` prefix.

## 2026.12.12 — 2026-11-27

For a model seat that must reach a multi-year goal from the tool surface
alone, such as a count in 1066 Ireland who would create the Kingdom of
Ireland, a king of Sweden in 867, or a ruler who must keep his line. Since
2026.12.11 the toolkit was played by model seats for many game-decades on
several starts, and each run's failures were reproduced from its recorded
replies and fixed. New server and bridge, released together. Still thirteen
tools by default (the four scheduled-run tools, `msg_send`, `note_read`,
`note_write` and `guide_read`, register only when a scripted environment
declares them). Items marked "not yet read live" are tested on recorded replies only.

**Steering the game's AI for your ruler**

* **`ai_veto`: the AI's own spending and politics, dropped for your ruler.**
  `do(intent="ai_veto", args={"op": "set", "what": ...})` makes the bridge
  drop, before the game runs it, what the game's AI would otherwise do for
  the seat's ruler: create a title, change a law, hire mercenaries, build,
  buy men-at-arms, call allies into your wars, pick a lifestyle perk, take a
  decision (all, or named keys), accept a vassal offer, declare a war of its
  own, change the court type, start or join a faction, buy a dynasty legacy,
  answer an incoming call to arms, or host an activity. `what="spending"`
  sets the kinds whose drop costs the seat nothing but the AI's own pick, in
  one call, and spares the titles the goal needs. Every dropped command is a
  news row with its price; an unvetoed AI act names the veto that would stop
  it. Vetoes carry to the heir when the ruler changes.
* **Event rules.** `do(intent="event_rule")` sets how the seat's ruler
  answers a recurring event (always, never, or `pay_if` a gold threshold,
  with an option the event really has), so a claim letter or a chaplain's
  bill is answered by rule between turns. Rules carry to the heir; every
  variant of a priced event is covered.
* **A rule for a whole family of events.** `do(intent="event_rule", args=
  {"op": "set", "event": "stress_threshold.*", "answer": "decline"})` (an
  `event` of the form `"<namespace>.*"`) sets, before any of them fires, one
  rule on each event of the namespace that has an option taking gold and one
  that does not, read from the game's event files: the free option, and as
  its else a free option the event offers. The reply lists each rule set
  (`guarded`), the events where the game's AI can still pick when neither
  named option is offered (`exposed`), and every event left without a rule
  and why (`not_guarded`); `op: "clear"` clears them. The turn's news keeps
  one short row per pick a rule decided (`rules_answered`), and says when
  your rule's else option answered rather than the game's AI. A rule's
  `else` is checked against the event's options whether or not the rule
  names an option. The `spending` veto also drops the two stress-relief
  decisions that take gold, and a decision's price follows the events its
  effect fires (not yet read live).
* **What an answer does, at the call that sets it.** An `event_rule` set
  on one event carries `option_effects`: for the option the rule names
  and its else, what the option does, read from the event's file and the
  scripted effects it calls (gold, prestige, piety, legitimacy and county
  control with their amounts, modifiers with their duration and what they
  change, titles, traits and factions), each on the scope it touches, with
  the condition of a branch (`when`) and `not_read` for effects the engine
  computes. `do(intent="faction", args={"action": "respond_demand",
  "response": "accept"})` (or `"refuse"`) carries the same for the demand
  letters of the factions now against your ruler (not yet read live). The
  read follows the scripted effects an option calls wherever the game
  defines them, the event's own file included (a faction demand's refusal
  had read "none"), and names what it fires (`trigger_event`: an event, a
  hidden letter to another character, an on_action, said to be the
  engine's own when common/on_action lists nothing for it), the truces it
  sets (`for`), and what its tooltip shows done another way, such as the
  faction's war (`shown_in_tooltip`). A branch's condition is cut at a word,
  never inside one, with its conditions joined by AND; a value the file
  writes as an expression (`scope:cost x -1`) is written out.
* **Your ruler's own levers.** A guest can leave an activity; men-at-arms
  can be disbanded by type; the ruler's perk points can be spent
  (`do lifestyle perk=<key>`); activities can be hosted through the game's
  AI with a pre-send check (pilgrimage proven live). Army disband, move and
  hold levers exist behind switches that default off; with army orders on
  (`CK3_BRIDGE_ARMY_ORDERS=1`), `do(intent="army", args={"action": "raid",
  "army_id": ..., "on": true})` toggles an army's raiding the way the army
  window does and reads the raiding flag back (not yet read live).
* **The AI's gold reserve.** `do(intent="gold_reserve", args={"amount": N})`
  sets the ruler's AI reserved-gold maximum (the game's own, whole gold;
  `move: true` also moves the gold held now into it); with no amount it
  reads the budget's buckets and the debt. Income fills the reserve first
  and the AI's budget-aware spending leaves it alone; forced charges still
  take it, so it is not a stop on AI buying (`ai_veto` is).

**Reads that say what the game will do**

* **Succession.** The heir line puts the holder's children first; heir rows
  carry their dynasty, with a warning when the primary heir is of another
  dynasty; the partition forecast says which son takes which title (one
  assignment, created duchies included); disinherit and vows are priced by
  the game's own check, and every reader gives the same answer. A diarch
  (regent) who blocks a lever is read, named in news and refusals, and can
  be dismissed at the target the game names.
* **What keeps a duchy in the realm, on the first page.** `succession_plan`
  carries `prevent_split` on page 1, after `breaker`: for each son who would
  leave the realm with land, what he takes and the disinherit and vows rows
  (price, whether the game allows it now, why not, what would allow it, and
  the call); the news rows about a split point at it.
* **Prices remembered per ruler.** A title's creation price is kept per
  ruler and title, and a read made for the bound ruler is kept under his
  own id. After a ruler change the briefing gives the late ruler's price
  (`was`, with the date it was read) beside the heir's and why it changed
  (`why_changed`, for example a discount of the late ruler's own that died
  with him); the realm's lost counties are counted from the late ruler's
  newest read.
* **Allies you cannot call.** `query(domain="alliances")` marks an ally who
  holds no realm (`not a ruler`: a courtier or a relative at court, whom
  the game's check refuses), a hegemon while you rank below emperor, your
  vassal (not callable, in the game's words), an ally under 100 military
  strength (the call is shown to an AI-run ruler only towards a stronger
  one), your liege or a ruler inside your realm, and lists the ones not
  callable under `not_callable` with their strength; an ally of your house
  carries a `house_member` note (the house call may be the one the game
  shows). `call_ally` refuses an ally who is not a ruler, a hegemon above
  your rank, a vassal or a dead ally before sending (not yet read live). An
  ally the reply lists under `forced_allies` is `is_forced` in his
  alliance row too, with `forced_read` saying the row's own flag read false.
* **A dead character is said first.** A read or call that names a dead
  character (`query`, `inspect`, the `do` intents that take a character,
  the marriage pair reads) answers `character_dead` before any other check,
  with the date of the read that first found him dead.
* **The acceptance of a call to war, per war.** `discover(what=
  "acceptance")` for `call_ally_interaction` and `join_war_interaction` runs
  the game's check once per war the send could name, with that war: one
  war gives its verdict with `war_id`, several give `by_war` rows, and the
  ready call is the war intent with its `war_id`. An interaction whose send
  names what the check cannot take (a title, a marriage's pair) is marked
  `not_checked`.
* **Hiring mercenaries against your purse.** The listing carries `purse`
  (gold, monthly income, the next title's price and how short you are) and
  each row its price in `months_of_income` (on the estimate) and
  `months_of_income_at_upper` (on the upper price). The estimate counts
  your own perks' and traits' `mercenary_hire_cost_mult` from the game's
  files beside your rank's (a duke holding soldiers_of_lesser_fortune_perk
  pays 30% less: the listing had read 166.73 for a hire charged 121); the
  modifiers it does not read are named in the price note. Known limit: in a
  live game the read of your own modifiers can come back empty
  (`your_modifiers: []`), and the estimate is then up to 30% above the
  charge (192.24 listed for a hire charged 135). The rows are the
  cheapest first, at most twelve, each a company's numbers only (what every
  row would repeat is said once, in `rows_read`); a listing with more
  companies in range says how many were left out (`cut`), names them
  (`cut_ids`) and gives the call that reads them (`read_them`:
  `do(action_class="mercenaries", candidates=[<company ids>])`, which now
  reads only the companies named), on every path a cut takes, and never
  shows a company without its price. `debt_after_hire` carries the game's
  `debt_line` (a hire may go into debt down to 24 months of income) and
  says per row whether the game allows that hire (`allowed`).
  The reads behind a hire still see every company in range. A hire the game
  refuses while the price, range and contract reads pass reads gate
  `engine_refused`, with what was ruled out, instead of `cannot_afford`.
* **What a defeat costs, on the declaration.** `declare_war`'s reply, and
  the rows that offer a war, carry `if_you_lose`: the gold, prestige and
  truce of yielding, from the game's peace-price table at your income.
  The prestige of each ending is read per casus belli from its own block
  in the game's files, for either side: a win as a claim or de jure
  attacker gives prestige experience, not prestige (a claim war's win had
  read +15), and a claim war's defeat is "-10 x the war's prestige
  factor (unread), at least -1000" with `prestige_not_read`: the game
  sets that factor for each war and no file or read gives it (it had read
  -15). The news prices a war's end for your ruler as defender too (an
  independence war lost: -150).
* **A death estimate.** The ruler's death is a calibrated band, not a
  date, with what his death would take (the titles that leave and the
  levers still open) on the reads that show the goal.
* **Wars.** Committed strength (what your open wars already use), the
  game's own declaration prices, priced defeats for every casus belli, the
  truce from the game's relation read, ducal-conquest rows per county,
  siege progress with its blocker and days left, raised armies and debt
  refusing a declaration before it is sent, and peace refusals with the
  score and what is missing.
* **Money and prestige.** The purse attributes gold, prestige and piety by
  cause in date order (your acts, the AI's, events, the monthly change),
  with months to a gold target at net income; levy upkeep and one-off costs
  are named; the kingdom bar is the game's at-least-half rule.
* **An act is charged in one step.** An act the server sent on the day a
  step ended, before that day's read, is charged in that step and not again
  in the next (acts and reads share one order). An answered event's piety
  and prestige are priced from its option in the game's files.
* **A cause in the ledger needs its record.** The purse names a cause only
  from a letter or an act it read, never from the shape of the numbers.
  The head of faith's gold-for-piety rows appear only when his accept
  letter is among the step's answered events (gold rising while piety
  fell had stood in for it). An answered event that is the letter an
  interaction sends when it runs is priced from that interaction's block
  in the game's files (gold, prestige, piety: an excommunication request
  costs 500 piety), with `sent_by` (your act, or the game's AI for your
  ruler, or another character). A spouse seat that changed holder is
  named, with `if_a_divorce` (the divorce's piety cost; a death costs
  none; which it was is not read). A piety change of more than 50 left
  unexplained carries `ruled_out` and `not_read`. A title gained whose
  price the gold fell by is no longer said to be the AI's creation when
  the bridge's record of the AI's posts was read and holds none
  (`not_created_by_the_ai`) (not yet read live).
* **The ledger's arithmetic, in one rule.** A purse row's `unexplained` is
  its change less every priced row of its step, each counted once: your
  acts, the monthly change, every `in_this_step` row with an `expected`
  and a matched price. A post the game's AI made for your ruler with a
  price (an `ai_acted` decision's 300 prestige) is now a row of the step
  and subtracted whatever its amount (it had been subtracted only when the
  fall matched it); a prestige row over 50 subtracts its monthly change (one
  had read the whole change as unexplained). Also priced from the game's
  files: an answered event's option that gives gold (a wedding's gift, a
  year's income, at least 75), and a realm law's pass cost (`expected` 0 for
  a law with no `pass_cost` block; a law that came another way names its
  cost as `pass_cost`, not counted).
* **A letter's sender, across steps.** An interaction's letter that comes
  in the step after its send names your act (`sent_by: you ...`): the ask
  the same news answers with that letter, or your act of that interaction
  up to 60 days before it. A call your ruler's AI sent that an acceptance
  letter answered is an ally who joined (an `allies_joined` row priced by
  his tier, `called_by` the game's AI), and "no ally joined a war you
  attack in" is never said beside an acceptance letter. A letter that
  several interactions send, or that the game's script also fires from
  elsewhere (an illness event), names them (`senders`, `elsewhere`) and
  counts no charge unless an act of yours names the one that ran.
* **Exclusive branches are one of them.** A charge read from an `if` /
  `else_if` / `else` chain (a call to war's prestige by the ally's tier) is
  one of its branches: `one_of` lists the totals it can come to and
  `if_its_conditions_held` is the largest of them (-750 for a call to war,
  which had read -1,335, the sum of every branch); a letter whose join an
  ally-joined row prices is `counted_in` that row.
* **Factions and threats.** A faction's strength against the ruler, its
  levers, and faction membership in the news.
* **Answers to your calls and your own wars.** A call to war is an ask: the
  news reads the ally's answer from the game's acceptance or refusal letter
  (joined, declined, the war ended first, or waiting), prices a join the
  letter records, and no longer rules out "an ally joined" when it could not
  read the letters. A war you declared and that ended between two reads is
  described as yours, with the date you declared it. The disinherit lever
  names the son whose check refused, beside the sons it allows.

**Replies a model can use**

* **Paging.** `observe`, `plan_war`, `succession_plan`, `inspect` title and
  `cbs` replies over the cap are served in pages, never cut; a page is kept
  per call until another call of the same kind replaces it, and
  `inspect(subject="more")` reads its next page (`CK3_REPLY_PAGES=off` gives
  back the single-reply size rule). Read replies
  stay within 8 KB. A purse row (gold, prestige or piety) whose change or
  unexplained part is 50 or more stays on `observe`'s first page, and so does
  the purse at a ruler change (`new_ruler`, now with the late ruler's last
  read beside the heir's, `predecessor_last_read`).
* **News that keeps the seat's own losses.** When `observe` must shorten,
  the rows about the seat's own losses stay. A briefing follows each ruler
  change; a ruler-change row says what the change cancelled.
* **Every refusal in words.** Unknown arguments are refused on every `do`
  intent and on declarations; refusals carry the game's own reason when it
  gives one; law refusals name the game's conditions; grant refusals name
  the liege chain.
* **The bridge's refusal codes, mapped.** Each refusal code the bridge's
  sources pass to a refusal reply is mapped to a gate, or listed as one no
  seat's call meets; a busy read slot
  is the new transient gate `read_busy` (retry), and a few codes carry a note
  on what to ask instead (a marriage read for an actor with no capital: ask
  again with `require_in_range=false`).
* **Shared notes.** `discover(what="notes")` serves short game facts and
  tool-use lessons. With `CK3_NOTES_SET` unset a server serves the default
  set: general game rules and tool use, no note scoped to one start, no title
  key in any note. A set names one start's notes (`CK3_NOTES_SET`):
  `ireland1066` (the 1066 Dublin seat, with a turn-one note that ends with
  the call creating `k_ireland`), `munster1066once`, `francia867`,
  `sweden867`, `lorraine1066`, `carinthia1066`. The Munster set leaves out
  five notes that describe the Dublin start: four scoped to Dublin, and
  `convert-faith-local`, which reports Dublin's ruler converting to Insular
  Celtic, the faith Munster's ruler already holds at the 1066 start (its
  scope now says Dublin). Notes and reply strings are written as rules: what
  the game does, in the present tense, a figure as a rule or an approximate
  range. `ck3agent/server/test_notes_general.py` defines what is served (each
  set's note texts, kinds and scopes; the string constants and f-strings of the
  server, the hooks, `ck3agent/cli.py` and the command line's helper modules,
  and the proposal mod's generator; the bridge's string literals, adjacent
  ones joined; the prompt files in `ck3agent/protocol/`; the two mods' game
  scripts without their comments; the tool descriptions) and refuses, in all
  of it, the narration forms it lists: a count of runs or games, a recorded
  case, a live case's tag, a character by its id, an in-game date other than a
  start's, runs or tests counted with a word between, a repeat count, one of
  thirteen past-tense verbs (fell, rose, came, ran, took, made, went, dropped,
  gave, refused, arrived, created, hired) with a figure after it, a live test
  or check, a probe as the evidence, an observed figure served as a bound
  ("up to about N gold") or a measurement ("measured: ...", "a measured
  average rate"), a release candidate's label, a save's character ids as an
  example; it also refuses a served text that names a script (`*.sh`, `*.py`)
  the tree does not hold. A story told in other words is not caught, and a
  figure from one game with no such words is not either. So each figure was
  checked by hand against its source for this release: a list of 286 rows
  over 251 served texts (the 245 a sweep found holding a figure beside a game
  quantity such as gold, prestige, piety, months, years, %, levies or opinion,
  or beside an estimate word such as about, roughly or measured, and six more
  notes read by hand), of which 279 hold a figure (the other 7 hold list
  numbers, format strings or none). Each is sourced to a key of the installed
  game's files (`common/`, `events/`, the defines) or to a constant or
  computation of this code, except four that rest on this project's own
  reading of the game's binary rather than its data files (the siege gate's
  strength ratio, the council pass interval) and the shader cache's size
  below; the sentence's scope was matched to the source's (which casus belli,
  which title tier, which era, which modifiers it leaves out). 50 were
  narrowed to their source's scope and 9 were dropped. A second pass checked
  the server's tables of bare numbers (the outcome table served where the
  game's files cannot be read among them) and the texts reworded after the
  first: 28 rows, 27 of them figures, 12 narrowed. The estimates left say
  they are estimates and their scope: the money read's small-realm
  shortfall (the code's own constant), the shader cache's size on a first
  start (not a game figure), and the reparations a peace-price or news row
  prices as years x 12 x the monthly income read (the game charges its own
  yearly income, which can differ). A note's
  provenance (a record of where the fact came from, not served) may name the
  games it was read in. The spending note names no count of the kinds it
  sets (the preset sets ten; it had said eight) and names activities among
  them; the Munster goal note states the reward in the prompt's words (your
  ruler first holds `k_ireland`; the line ending, or passing to a ruler of
  another dynasty, ends play). Each note's provenance says its source.
  Anything that froze a notes version must re-freeze: every set's version
  moved.
* **What a client may tell the server.** `CK3_TOOL_PREFIX` (the prefix the
  client puts before the tool names: the calls a reply suggests carry it)
  and `CK3_GOAL_TITLE` (the title the run's goal names: the create_title veto
  never drops it, and a cut reply keeps its `formable_titles` row).
* **Plain words in what is served.** Refusals, the `session` description,
  the run prompt and `ck3agent run --help` say "a scheduled run" (a harness
  that runs several seats on one schedule, the docs' "Scheduled runs") where
  they said "an experiment cell", and "this run" where they said "this
  experiment"; the decisions refusal and the money read's two absence notes
  carry no tracking numbers. The war-stance reply's surrender cost names
  the defeat of the claim, de jure, conquest, vassalization, tributary and
  holy-war families as their files give it (a claim or de jure war: 10 x the
  war's prestige factor, legitimacy and 3 years of income, and a claim war's
  claim is lost; a conquest: 150 or 350 prestige, legitimacy and 2 or 3
  years; a vassalization war: 350 and 3 years; a tributary war: 350; a holy
  war: no prestige, legitimacy and 2 to 4 years) and says other casus belli
  are not listed, with `end_war`'s check giving one war's price; the
  claim-war yield's line says the game charges 3 years of its own yearly
  income and that the row's 3 x 12 x the monthly income read can differ from
  it (it is served under `rule`, where it was under `measured`), and every
  line that prices reparations from the monthly income read says the same of
  its own years. A won claim or de jure
  war gives its attacker prestige experience (fame), not prestige, in the
  peace-price rows as in the outcome table, whose conquest rows now give a
  win 0 prestige (the 75 and 150 there were prestige experience). Known
  limit: the row can misprice that gold (in one game it priced a yield at
  171 gold and the game took about 121, so the game's yearly income for
  reparations can differ from 12 x the monthly income read); the war-end act
  records the charge the purse read shows, and the row's price only when no
  charge was read. The truce the peace-price rows and the replies of
  `end_war` and `make_peace` state is the casus belli's own, from its
  endings in the game's files: the one-way truce of 1825 days for the
  priced casus belli whose endings call the game's standard truce effects;
  for a refused liege demand war, a truce both ways of 1825 days on its
  victory or white peace and none on its defeat; any other casus belli's
  truce reads "not read" in `make_peace`'s reply (the peace-price table
  prices no other). They had stated the one-way truce for every casus
  belli. The fabrication's rate names the terms that slow it (x0.5
  off your border, x0.3 on a county held inside your realm, x0.8 with a rival
  chaplain, x0.7 with a nemesis): the most months it states are for a
  chaplain who is neither, and `overdue` is set only past the most a nemesis
  allows and only once the county's holder was read and is outside your
  realm. A county held inside your realm is priced at its x0.3 alone, and the
  reply says the months may be up to twice that: the game's files do not say
  whether it also takes the x0.5 off your border there. A
  coping trait's gold is 50 or more (its file's floor); the crown-authority
  price is said before faith and culture modifiers. Notes: a claim war's
  declare price per title tier (100 a county, 200 a duchy, 400 a kingdom,
  800 an empire, a quarter less pressed); a white peace's and a yield's
  prestige for a claim or de jure war; a war won as attacker gives fame, not
  prestige; disinheriting a son who would take a share (300 prestige, 150
  renown x the era factor); a hunt's prestige by quarry and its main
  modifiers, saying other modifiers exist; the truce's usual length, less
  with the flexible truces perk; a hegemon called into your war costs no
  prestige (the call's effect has no branch for him); the 867 start's empire and king
  notes give the rule, not that start's figures. Anything that froze a notes
  version must re-freeze again. The refusal codes and the 13 tools, with
  their names and parameters, are unchanged. The goal
  block outside a scheduled run reads `{"state": "no_scheduled_run"}` (it read
  `not_an_experiment_episode`).

**Bridge**

* **Hires and recruits by a ruler born during the game.** The bridge
  passes the game whole character and company ids for the mercenary hire,
  the men-at-arms recruit, the raid order, the council seat swap and war
  creation. It had passed the low 24 bits only, so for a ruler born during
  the game his hires were refused, and his recruits, raid orders, seat swaps
  and war creations named a character the game could not find.
* **A scope fix without an engine lock.** The bridge's temporary script
  scope no longer takes the engine's random-scope lock (it could deadlock
  the game); a restore it cannot make yet is deferred to the next safe point
  instead of latching, and counters record each deferral.
* For a scripted environment only (not reachable from the public tools):
  a speed the environment declares at launch lifts the bridge's cap at 3
  (4 and 5 checked live), and it can arm the bridge's pause alarm at each
  check date. From the tools, `session` still takes speed 1 to 3 and refuses
  anything else.
* The Windows build has the veto and event hooks and the newer levers; it
  builds with clang-cl and the MSVC libraries (xwin) from Linux (docs/porting), CI builds its
  platform layer with MSVC, and it is not yet proven on a live Windows game.
* The offset header (`re/offsets/ck3_offsets.h`, and the Windows headers
  generated from it) says each fact with its confidence and evidence, without
  the research process's round tags, labels, process ids, dump names and
  dates. Since 2026.12.11 it
  also holds the offsets this release's levers and reads use, and some values
  were corrected, each with its evidence in the comment (among them the two
  vtable slots of the mercenary hire command, which had been recorded the
  wrong way round). The bridge compiles the header's md5 into its build
  provenance, so this build reports a new `build_offsets_md5`.
* The bridge's log lines and replies were reworded to carry no tracking
  numbers and no design-stage labels (the served-text test above reads its
  string literals for the spellings it lists; the create_title hint names "the bridge's own
  de-jure-majority bar"; the pipeline-mode refusal names no design tag; the
  casus-belli option buffer's two log lines say "CB option buffer full"; the
  dispatch-context line, and the init, tick and drain lines, carry no stage
  prefix; the casus-belli query's gate lines name the gate: "group-trigger
  gate", "relation gate"); a port phase a Windows or macOS line names is the
  port plan's (docs/adr/0001); the scoring-hook dispatch's log lines start
  `DISPATCH score_hook:` (they started `DISPATCH_V5:`). Wording only: a log
  reader keyed on `DIAG tick=` reads it as before.
* **Parents read the right way round.** The family record's father and
  mother ids were recorded in the offset header each in the other's place:
  every read of a character's `father_id` and `mother_id` (the character
  reads behind `observe` and `query(domain="character")`, and the pair reads
  of the marriage and relation checks) had given the mother as the father and
  the father as the mother. They are corrected (the header's family record,
  with its evidence).

**The tool surface since 2026.12.11** (every difference of the 13 tools'
parameters and enums, read from the generated tool reference of each
version):

* **Breaking: `query` lost six parameters**, the `map_graph` domain's options:
  `scope`, `hops`, `max_nodes`, `target_county`, `target_county_key` and
  `counties`. A call that passes one is now refused as an unknown argument;
  `query(domain="map_graph")` is still served with its defaults, and
  `inspect(subject="map")` is the region read.
* `query`: `title_id` takes an integer or a title key; `detail` enumerates
  `brief`, `full`; `sort` and `sort_order` enumerate their values (`age`,
  `diplomacy`, `gold`, `intrigue`, `learning`, `martial`, `military`,
  `prowess`, `realm_counties`, `realm_size`, `stewardship`, `tier`; `asc`,
  `desc`).
* `discover`: `what` adds `notes`, with a new `topic` parameter; `action`,
  a free string before, now enumerates its values in the schema
  (`back_claimant`, `banish`, `claim`, `declare_war`, `execute`,
  `execute_prisoner_interaction`, `imprison`, `imprison_interaction`,
  `press_claim`, `revoke_title`, `revoke_title_interaction`, `war`), and so
  does `decision_type`, listing the two decisions `decision_why_blocked`
  models (`found_kingdom`, `negotiate_the_danelaw`, each also with
  `_decision`); the server reads both as before and does not refuse another
  value; `title_id` and `location_id` take an integer or a key.
* `do`: `intent` adds `ai_veto`, `event_rule`, `gold_reserve`,
  `host_activity`; `action_class` adds `activities`, `income_levers`.
* `inspect`: `subject` adds `map`, `more` and `threats` (`inspect(subject=
  "threats")`: who can declare war on your ruler now and on what, the game's
  own casus-belli check asked with each nearby ruler as the attacker, his
  strength against yours, and the vassalization rule with your county
  count); a new `layers` parameter (the map's columns); `title_id`
  takes an integer or a key.
* `observe`: `depth` adds `news`.
* `cbs`: a new `include_invalid` parameter.
* `declare_war`: `cb_title_id` takes an integer or a title key.

**Earlier in this release**:

* **A map of the region in one reply.** `inspect(subject="map")` gives one
  line per county of a kingdom (the seat's own, its target, or the one
  named) and the counties bordering it, in the game's de jure order, with
  columns by `layers` (holders, casus belli, armies, strength, adjacency,
  duchy, land); a value no read gives prints `?` and says why once, and
  each row's deep-dive call comes with the reply.
* **A de jure title in one call.** `inspect(subject="title",
  title_key="k_ireland")` answers one row per de jure county (id, key, name,
  duchy, holder, the ruler at the top of the holder's liege chain with his
  realm's military strength, whether you control it, your claim on it) and
  a `next_call`: the `declare_war` arguments when the game's casus-belli
  check says a war for it is declarable now, else the gate, else the
  claim-fabrication council task. Beside them: each de jure duchy, each
  realm, and `summary` (counties held of total, the counties creation
  needs). A field that was not read is null with the read named in
  `errors`, never defaulted.
* **Title keys wherever a title id is taken.** `title_id`, `cb_title_id`,
  `target_title_id` and `location_id` take a key (`c_dublin`) as well as an
  id, in tool arguments and in `do`'s `args`; the reply names each title it
  resolved (`titles_named`). An unknown key is refused
  (`target_not_assignable`), never sent as 0.
* **Every creation requirement at once.** `do(action_class=
  "formable_titles", candidates=[...])` (candidates now required) gives each
  title's `requirements`, met or unmet: de jure counties held against
  needed, the price against the gold held (read even while the county bar is
  unmet), and the game's own creation check in its words. The game's tier
  rule (one of: a duke holding enough duchy titles, or a title of kingdom
  tier or above) is one `tier_rule` row read off the game's own lines,
  saying what it asks of you at your present tier ("become a duke holding 2
  duchy titles (or hold 1 title of kingdom tier or above)" for a count),
  met or not, with what you hold; the game's lines had read as four
  separate requirements. When it asks for duchy titles you lack, the row
  names the cheapest duchies the same reply priced and the whole bill (on
  the 1066 start: 212.50 gold for each of two duchies and 425 for the
  kingdom, 850). Without those lines it is `met: null`, unknown. `mint_title`'s refusal carries the same list,
  and takes `args.title_key` only. `observe`'s `de_jure.next_title` names
  the next title up that the ruler does not hold, with its requirements; it
  replaces `can_mint_now`, which was true for a title already held.
* **One answer to "can I declare this war now".** `cbs`,
  `discover(what="war_targets")`, the title table's `next_call` and
  `observe`'s news all judge a war by the same casus-belli read. A
  `war_targets` row is `declarable` only when that read says so, with its
  `declare_args`; otherwise it carries the gate (`declarable_authoritative`
  is gone). On a live 1066 game the old list offered county-conquest wars
  that `declare_war` then refused; and after a fabricated claim arrived its
  claim row said there was no claim while `cbs` offered `claim_cb`: a claim
  row now matches the claim casus belli on that title.
* **Claim targets you name.** `do(action_class="fabricate_targets")` takes
  `args.within` (a kingdom or duchy key) or `candidates` (county keys) and
  answers each county: yours already, a claim you hold already, or a target
  with its ready council-task call (the game's own check of that target is
  not read; the council task's reply carries any refusal). Without them it answers the bridge's
  county list with a warning (`targets_not_ruler_specific`): on a live 1066
  game that list was the same 40 counties, none in Ireland, for three
  different counts.
* **News in `observe`.** A `news` block of what changed since the server's
  last `observe`, each row with a `next` call: claims gained (with the
  claim war's `declare_args` when it is declarable now) and lost, wars
  started and ended (with the outcome from the war-end ledger), ruler,
  liege and heir changes, council seats whose holder or task changed (a
  claim fabrication that left the chaplain names the next fabricate call),
  schemes ended, titles gained and lost. A read that did not answer makes
  its rows `unread`; an outcome no read gives is `null` with the reason.
  The brief also carries military strength (levies, men-at-arms), the
  liege, the council, and the money unit.
* **Sent is not refused.** A sent offer whose effect the game applies later
  (calling an ally into a war) is `pending` with where to read it, never
  `recipient_refuses`. `discover(what="acceptance")` runs the send's own
  checks, so it and the send agree, and says `sendable`; `interaction` and
  `interaction_type` are the same argument. The war-stance stall verdict
  reads the same sieges `inspect(subject="wars")` lists; occupation 0 with
  no siege is `null` (`stall_unknown`), not a stall. No reply is `ok: true`
  while it carries a handler's error.
* **Small default replies.** `cbs` lists the valid options whole and counts
  the rest by gate (`invalid_by_gate`; `include_invalid=true` lists them).
  The interaction and decision lists are capped at 40 names with the rest
  counted and take name fragments to filter; the fabricate list and
  `war_targets` are capped at 24 rows. On recorded replies from a live 1066
  game, every one of these defaults is under 8 KB (`cbs` from 36 to 38 KB
  to under 3 KB; the interaction list from 82 KB to under 6 KB).
* **Truer descriptions and schemas.** Every closed value set is an enum in
  the schema; numbers in the text come from the code's own constants. Also
  new: `do(action_class="income_levers")`; `end_war` with
  `check=true` says what each term needs; one sentence on armies (no tool
  raises, moves or disbands them: the game's AI does).
* **One shape for a ready call.** Every field that hands you a call
  (`do_call`, `next_call`, `next`, `fix`, `check_first`, `enumerate`) is
  `{tool, args}`: a `do_call` is `{"tool": "do", "args": {"intent": ...,
  "args": {...}}}`, and the title table's `next_call` no longer names the
  tool as its key. A scripted driver had broken on the two shapes.
* **Marriage candidates you can send.** Each listed row is paired with a
  spouse-to-be on your side the fields allow: you or one of your children,
  unmarried, of age, of the other sex (the ruler, his children and each
  candidate are read for it). Rows that cannot be paired are counted by why
  (`not_listed`), at most 10 rows are listed, and the reply is under 8 KB
  (17 KB before). The game's own send check then judges each listed row
  with its pair (nothing is sent): a row the game would refuse, or whose
  recipient would answer no, leaves the list and is counted under the
  game's reason (a few are named in `excluded`); a listed row carries the
  game's `answer`, `accept_score` and chance. Faith and kinship are applied
  by that check. When the check does not answer, the rows are listed
  unjudged and the reply says so. (An earlier build in this release dropped
  the check because, without the pair, the game refused every marriage.)
* **The head of faith's claim.** The bridge now sends
  `hof_ask_for_claim_interaction` through its own pipeline with the title
  passed as `args.title_id`, so the send check can judge it (before, it
  went through the AI-scoring fallback, ignored the title and was refused
  `ai_no_valid_target`). Its interaction-list row carries the rules of the
  game's own interaction file: name the title's holder as `target_id` (the
  interaction redirects to your head of faith), it is shown only when the
  holder shares your faith, it costs 100 piety on a county (200 a duchy,
  400 a kingdom) plus 10 per county of your realm, and it gives a pressed
  claim. A send to a holder of another faith is refused before it is sent.
  `do(intent="interact")` still warns `title_not_applied` when a title is
  passed to an interaction the bridge does not send itself. Not yet run in
  a live game.
* **News: baronies.** A barony gained in the same step as its de jure
  county takes the county's `how` (`with_county`); one gained later stays
  unknown.
* **Fixes.** A game speed above 3 is refused and nothing is written (it was
  set to 3 and answered as applied); `query` refuses an unknown characters
  sort or filter (the bridge read them as other values); a frame with no
  request id, or another call's drain record, is never taken as a call's
  reply. With `CK3_CLOCK_OWNER=env` (an environment owns the clock),
  `session` does not list pause, unpause, speed or wait and refuses them
  (`clock_owned_by_env`).
* **A war score is the war's total.** Every reply that shows one (`inspect`
  `war_stance`, `wars` and `war_score`, `query` `wars`, `plan_war`'s active
  wars, `observe`, the refused `make_peace`) names the total the game uses
  for peace terms `war_score`, with its parts (`battle`, `occupation`,
  `imprisonment`, `ticking`) under `war_score_parts`. When the game has not
  computed the total yet it is `null` with `war_score_unread`, never the
  battle part (`query_wars` had sent battle plus ticking under that name).
  Once the total reaches what `we_win` needs for your side, the row carries
  the ready `end_war` call; `end_war check=true` does too when the game says
  `we_win` is open. A model seat had waited in a war whose score it read as
  the battle part only.
* **News reaches the seat every turn.** `observe(depth="news")` answers only
  the news, compact, since the last `depth="news"` read; it does not move the
  news of the seat's own `observe`. News also says when the prestige, piety
  or gold changed (with the acts this server sent whose cost it read; the
  cause is otherwise `unknown`) and which wars are at `we_win`.
* **Council targets read as what they are.** The game stores a county
  council task's target as the county's capital province; the bridge named
  it as a title with that id (the chaplain fabricating on Athlone read
  "Blue Horde"). `query(domain="council_tasks")` now gives
  `target_province_id` and `target: "unknown"`, or the title for a
  fabrication this server set (it reads the province back when it sets it).
* **The claim fabrication.** The council reads (`query` `council_tasks`,
  `inspect` `council`) and the news say whether a fabricate task runs, on
  what, since which date this server saw it, and how many days; its
  progress is `null` (the bridge reads none). A fabrication this server set
  that the game's AI stopped is said loudly, with the call that sets it
  again; one that ended with the claim held is not. The `council_task`
  reply carries the game's rate for the task: its terms, and the most months
  it takes for a chaplain who is neither your rival nor your nemesis.
* **The cost of declaring.** `declare_war`'s reply carries the casus
  belli's cost (`declare_cost`) as the `cbs` and `plan_war` rows showed it
  before the war.
* **A war you cannot pay for is not offered as declarable.** The game's
  casus-belli check does not ask whether you can pay: on a live 1066 game
  `cbs` and `plan_war` offered a claim war that the game then refused
  without a word, twice while the ruler was in debt and once with his
  prestige below the war's cost. Every place that judges a war (`cbs`,
  `plan_war`, `discover(what="war_targets")`, the title table's
  `next_call`, the news) now checks gold below zero, the claim war's
  prestige cost (from the game's own cost table: 100 a county before the
  game's multipliers, 75 if the claim is pressed; about 125 measured on the
  1066 start; the bridge had priced it at 0) and a truce. `cbs` lists such
  wars under `blocked_now` with what is missing; `declare_war` refuses them
  before sending, naming the condition and the amount; and when the game
  refuses a war these all pass, the refusal lists what was checked and what
  was not. The bridge's cost estimate for other casus belli is shown, never
  a refusal.
* **Where the purse went.** News prices what it saw move the prestige,
  piety or gold in the step: a fabricated claim arriving (the game charges
  (40 - the chaplain's learning) x 3 gold, at least 40-60; four such claims
  took 79 to 101 gold each on the 1066 start), a war declared or ended, a
  realm law passed, a county gained, and the expected income at the monthly
  rate; what is left is `unexplained`, and a change nothing priced stays
  `unknown`.
* **`idle` in the news.** Facts the reads hold that nothing uses: vacant
  council seats, no claim being fabricated, claims with no war, no
  alliance, piety of 100 or more; each with the call that lists what can
  be done, absent when none holds. A model seat had spent 19 of 46 turns
  with no call while one claim was being fabricated.
* **Appointing to your council.** `do(intent="appoint")` sends the acting
  ruler (it had sent none, and the bridge read the courtier as the actor:
  `actor_not_claimed` on the seat's own council) and takes the seat by its
  name (`steward` or `councillor_steward`). A vacant seat is refused before
  sending: the bridge's council write only swaps two occupied seats, and
  the game's AI fills vacant ones.
* **The creation bill in the title table.** `inspect(subject="title")`'s
  `creation` prices as many duchies as the tier rule needs (the nearest
  first, creatable or not), so its bill agrees with `formable_titles` (it
  had read "unread" with one duchy priced).
* **What creating the title takes, in the title table.** `inspect(subject=
  "title")` for a title you do not hold carries `creation`: counties held
  and needed, the tier rule, the price and the bill, and the nearest unmet
  requirement with its ready call (on the recorded Ireland state: create
  d_meath, 2 of 2 counties held, 212.50 gold of 229.67), from the same
  function as `formable_titles`; two create-title checks in one span.
* These last twelve items are tested on recorded replies from live 1066
  games, not yet read live.
* Read live: at the 1066 start (CK3 1.18.3.1), a scripted policy drove
  this release's server from its replies alone through a fabricated claim
  on Athlone, the claim war and the county gained. The changes above that
  were made after that run (the tier rule row, `war_targets` claims,
  marriage pairs, the one call shape, the head of faith row, baronies in
  the news) are tested on the replies recorded in it, not yet read live.

## 2026.12.11 — 2026-11-19

From a 25-year seated pilot on 2026.12.10: a king's `observe` took about
30 seconds and most of his vassals' realms came back unread, and from
1088 on the game's log no longer carried the lines that confirm a scripted
action ran. A king's vassals are now read in one bridge read; our script
lines are written at INFO and the log channel is read, so a dark channel is
named instead of reported as a write that did not apply. The Windows MSVC
build is green and gates CI. The surface is still thirteen tools.

* **A king's vassals in one read.** `query_realm` takes `character_ids`
  (1 to 64 ids) and answers each id's own realm reply from one walk of the
  game's character table (`bridge/include/realm_tally.h`), where each
  vassal's read used to be its own walk of about 0.9 s. `observe` sends that
  one read for every direct vassal (`pushed_warnings.vassal_read_request`).
  On the 1066 save, paused, a king with 32 vassals observed in about 1.7 s
  instead of about 13 s, with no vassal realm unread (`vassal_realms_unread`
  was 16 of 32 before). A list that is not 1 to 64 positive ids is refused
  whole (`too_many_character_ids`, `invalid_character_ids`,
  `ambiguous_character_ids`); an unknown id answers `character_not_found`;
  an id past the game-thread budget answers `not_reached`. **The bridge and
  the server ship together:** a 2026.12.11 server on a 2026.12.10 bridge
  reads no vassal realm, and says so (`vassal_realms_unread`, every vassal,
  "a reply without a realms list"); it never falls back to one read per
  vassal.
* **The script log channel.** The game stops writing a log level once
  100,000 messages of that level were logged in a session. The marker lines
  that confirm a scripted action (gift, sway, council task, `declare_war`,
  `set_policy`) ran, and the policy and proposal mods' lines, were DEBUG
  and went dark in a long game; they are now INFO (`info_log`), and the
  policy mod no longer logs a line each time it re-arms. `script_action` and
  `query_script_log` carry `script_log_channel` (`open`, `closed` with
  `closed_since`, or `unmeasured`, with the INFO and DEBUG counts and the
  cap). A marker not seen while the channel is not read open is reported
  `log_unreadable` (guard) and `script_log_unreadable` (pending), never
  `write_not_applied`; an incoming-proposal list read from a closed channel
  is `script_log_unreadable`. Not measured on Windows: the bridge answers
  `unmeasured` there. In testing the cap was reached once naturally (on
  2026.12.10's DEBUG lines, about 16 game-years with ten seats) and
  otherwise by writing the game's counter; no natural 50-year run has been
  made. Reinstall the policy and proposal mods
  (`docs/standing-policies.md`, `docs/incoming-proposals.md`) to get the
  INFO lines.
* Still not seen in a running game: an AI white-peace offer held, and an
  answer to a voided offer read back.
* **Windows: the last MSVC error fixed.** The one error real `cl.exe` gave
  on 2026.12.10 (C2440 in `bridge/src/query_handlers.cpp`, a
  `reinterpret_cast` from `uintptr_t` to `uintptr_t`, which clang-cl accepts)
  is fixed. The clang-cl build's `msvc_standin_lint` now also reads clang's
  AST of every Windows translation unit and fails on a `reinterpret_cast` from
  an integer or enum to its own type; its reader is tested on every Linux
  build (ctest `msvc_standin_lint_selftest`). Three Windows tests that only
  real Windows with MSVC reds are corrected: the proxy-refusal host is built
  beside its `version.dll` under the Visual Studio generator, the
  guarded-region test's progress marker is `volatile`, and the game-log test
  compares long path names. The CI's installer-parse step itself no longer
  fails to parse (`"${f}: $_"`; PowerShell read `$f:` as a drive-qualified
  variable). No behaviour change on Linux.
* **CI: the Windows MSVC job now gates.** It has been green on real MSVC, so
  it no longer runs with `continue-on-error`. The non-gating "Python unit
  tests" steps on the Windows and macOS legs are removed: the MCP server and
  the CLI are Linux-only (they use `fcntl`, POSIX signals and Unix sockets), so
  on Windows the suite never collected. Both legs still run the portable
  `tools/inspect_binary.py` tests.
* **A tidier repository.** Comments in shipped code no longer carry the
  research tree's internal tracking labels; code, strings and line numbers are
  unchanged. The Windows VM runbook, a plan that was never run, is withdrawn
  (the ADR's P7 row says what that run needs). The docs index is renumbered,
  and every relative link and anchor in shipped Markdown is now checked when
  the repository is built. No behaviour change.

## 2026.12.10 — 2026-11-17

From multi-seat runs on 2026.12.9: several seats in one game read and wrote as
each other's rulers, a council task the seat set was reassigned by the game's
AI, the bridge's one reader thread fell behind under a seat's read batch, and
every offer the AI made to a seat's ruler was answered by the AI. Offers can
now be held for Claude to answer (experimental, with a second small mod that
ships in the tarball); a seat can end a war it leads. The surface is still
thirteen tools.

* **Incoming proposals (experimental).** With the proposal mod installed and
  the server's environment declaring it (`CK3_PROPOSAL_MOD=1`), an AI's
  marriage, alliance, call to war, peace or vassalization offer to a ruler
  Claude plays is held. `observe` lists it under `incoming_proposals` (who
  offers what, its id, the default at the lapse, the exact call to answer
  it); `do(intent="answer_proposal", args={"proposal_id", "answer"})` accepts,
  declines or hands it back to the game's AI (`native`, for marriage,
  alliance and vassalage); `do(intent="set_policy", args={"policy":
  "proposals", "class", "answer"})` sets a standing answer per class. An
  answer is checked against the interaction's own conditions immediately
  before it is carried out: an offer that no longer meets them is void,
  refused `interaction_invalid` (`invalid_now`), never reported applied. An
  answer to an offer the game voided is refused `proposal_closed` with the
  void's reason (`void_sender_dead`, `void_subject_dead`, `void_war_ended`,
  `void_recipient_dead`). An offer is held at most 400 game days. Without the
  declaration nothing changes (`proposal_mod_not_declared`); with it and
  without the mod enabled in the game, the calls are refused
  `proposal_mod_not_loaded`. The tarball carries the mod under
  `proposal_mod/`: its fixed files and `proposal_overrides.py`, which
  generates the interaction copies from your installed game's own files
  (they never ship); install by hand: `docs/incoming-proposals.md`. Held and
  answered in a running game: the AI's own marriage offers and calls to war.
  Not yet seen in a running game: an AI peace offer (white or otherwise)
  held, and an answer to a voided offer read back.
* **A seat can end a war it leads.** `do(intent="end_war")` with `terms`
  `we_win` (needs your war score at 100), `white_peace` (needs the enemy's
  below 25) or `we_yield`, against the enemy war leader, through the game's
  own script and its own checks; without `terms` it is a read of what the
  game would allow. `applied` means the war is gone from the war list. The
  enemy is not asked.
* **The war arguments are checked before the game is asked.** A casus belli
  name that is not a type (a group name such as `claim`) is refused
  `cb_not_a_casus_belli_type`; a claim war on your own claim is usable
  whether or not the claim is pressed (another claimant's must be pressed),
  in the bridge's claim reads, `plan_war` and `declare_war` alike. A council
  task whose character target holds no title is refused
  `council_target_not_addressable`.
* **A council task you set stays set.** The game's AI reassigned a seat's
  council task on the ruler's periodic pass; the bridge now skips that pass
  for the position your own council-task write set, while you control the
  ruler. Replies carry `ai_pin`.
* **A seat never acts as another seat's ruler.** A write that names no
  ruler acts for the calling session's own ruler or is refused `no_actor`,
  and so does a seat's read; neither falls back any more to the ruler of the
  first session that claimed one. A write that names its ruler under a key its verb does not read
  (`character_id` where the verb reads `actor_id`) is refused
  `actor_key_not_read` and writes nothing.
* **The bridge serves every client in turn.** Its reader serves one line
  per client per round, so a seat's batch of reads no longer holds up other
  clients' calls; a ping or broadcast to a client that is not reading is
  dropped instead of waiting on it. The server drops a bridge connection
  whose handshake was never acknowledged and reconnects with its kept
  token.
* **Reads that come back empty are named.** `observe` names vassal realm
  reads that returned nothing (`vassal_realms_unread`); an unread policy-mod
  check is a read failure (`policy_mod_check_unread`), never "not loaded";
  `observe`'s war rows carry each side's participants (`attackers`,
  `defenders`).
* **A write the game refused is not resent that month.** The identical write
  in the same game month is answered from the earlier refusal (`repeat_of`,
  nothing sent), and `plan_war` stops offering it (`refused_this_month`).
* **`ck3agent run`** stops a Claude start that came up without the game's
  tools and restarts it once (a second such start stops the run,
  `seat_surface_failed`); its Claude gets a 60 s MCP start timeout unless you
  set `MCP_TIMEOUT`. A run's `actions.jsonl` keeps only its own sessions' rows.
* **Windows.** The bridge's Windows sources build without GNU-only
  spellings (a portability check runs in the test suite), and the script
  action layer has its Windows anchors. The Windows build is still not a
  release asset.

## 2026.12.9 — 2026-11-12

From the review of what the v3 seats actually did: every war the game refused
had been offered by `plan_war` or `cbs`, a succession was called intact from
the primary title alone while a second kingdom went to another heir, and
nothing kept a seat inside the realm it started in. Four writes now
go through the game's own script, guarded by the game's own check; a vassal
can hold a standing policy between turns (with a small mod that ships in the
tarball); `observe` gains two blocks. The surface is still thirteen tools.

* **Gift, sway, council tasks and `declare_war` run as the game's own
  script, behind the game's own check.** The bridge has two new verbs,
  `script_action` and `query_script_log`. `script_action` takes a fixed
  template name and ids, never script text: `gift` and `sway` (the
  interaction with its own validity check), `council_task`, `declare_war`
  (`start_war`, guarded by `can_declare_war` for exactly that defender,
  casus belli, claimant and title), `set_policy` and `clear_policy`.
  `do(intent="interact")` with `gift_interaction` or `sway_interaction`,
  `do(intent="council_task")` and `declare_war` use it; the reply's result
  carries `path: "script"`, the `template`, the `guard` (`ran` or
  `refused`) and `game_errors`, the game's own error text. A war the game's
  check refuses is `cb_invalid_for_target` (`can_declare_war_false`) with a
  fix that points at `cbs` and `plan_war`. A script that ran and changed
  nothing is `write_not_applied` (`silent_no_op`), never applied. A
  character the game's script cannot name (no held title) still goes the
  native way. Every other intent is unchanged.
* **`plan_war` offers only wars `declare_war`'s pre-check passes.**
  `plan_war` and `cbs` now read the casus belli as your bound ruler (a vassal's call used to
  be read as the bridge's other controlled ruler) and refuse
  `actor_unresolved` when he cannot be read. `plan_war` runs `declare_war`'s
  own pre-check on the casus belli each row offers: the row's `precheck`
  (`passed`, `as_of`, `not_checked`, `limit`) and `precheck_by_cb` (replacing
  `ready_to_declare_by_cb`). A casus belli that did not pass, a war on your
  own liege and a claim whose claimant is not in your court are listed in
  `excluded`, never offered.
* **Standing policies.** `do(intent="set_policy", args={"policy": ..., "on":
  true|false})` sets `join_liege_wars` (join each war your liege leads, when
  the game's own join-war checks allow it) or `stay_out_of_liege_factions`
  (leave a faction against him and join no other). The game re-checks a
  policy every 3 days, between your turns. What holds it is a small mod,
  `ck3agent_policies`, now in the tarball under `policy_mod/` with the steps
  to install it by hand (`docs/standing-policies.md`); without it
  `set_policy` is refused `policy_mod_not_loaded`. `observe` carries
  `standing_policies`: each policy on or off, `mod_enabled`, and how to set
  one.
* **`observe` has a `goal` block.** In an experiment episode it gives the
  score (the county count of the played line's realm: now, at the open and
  the change), the horizon, the cycle and, for a duke seat, his own
  counties. Outside one it reads `{"state": "not_an_experiment_episode"}`.
* **The succession verdict checks every crown title.** `succession_plan`
  and `observe`'s succession block list `crown_titles` (each kingdom and
  empire held, with the heir the simulation and the per-title map each give
  it; the map's heir for one is a breaker) and `scope` (a title gained after
  the read is not in the verdict). A crown title with no heir read makes the
  outcome undetermined.
* **A seat is never handed a ruler outside its realm.** The bridge records
  the seat's realm (its ruler's top liege, read alive, `realm_root_id` in
  the session list). When the ruler dies and his heir is outside that realm,
  the seat is unbound with `heir_outside_realm` instead of following him; a
  claim made while the seat's ruler is dead is held to the same check.
* **The played line can end.** The bridge's watch of a played line
  (`query_character` with `played_line`) reports `line_ended`, dated
  (`ended_date`), only when the game gave the line's title to nobody and the
  dead ruler holds no title any more.
* **`ck3agent run`** records every turn's prompt (`<stamp>.turns.jsonl`:
  the game date, the cycle, the text and its sha256, the messages delivered
  at that turn) and what the live-state hook injected at each prompt
  (`<stamp>.hook.jsonl`). The manifest's `total_cost_usd` is now the last
  value of each Claude session, summed over sessions (a resumed session
  reports its cost cumulatively, so the old sum overcounted), stamped with
  `total_cost_usd_rule`.

## 2026.12.8 — 2026-11-10

From test runs on 2026.12.6 and 2026.12.7: `inspect(subject="war_stance")`
answered `stance "unavailable"` for 29 of 32 reads, all after the war was
declared, and labelled that placeholder as the engine's. The surface is
still thirteen tools; `ck3agent run` is unchanged.

* **The bridge reads the engine's war stance by default.** The per-war
  stance comes from walking the game AI's war-coordinator records. That walk
  was off unless something sent `set_war_stance_record_walk`, and nothing in
  the toolkit did, so every row carried the bridge's placeholder. It is now
  on from the start (it only reads memory, bounded). `query_war_stance`
  reports `record_walk_enabled` true on a fresh bridge, and a war the
  character leads as primary attacker gets the engine's stance name
  (`attacker_offensive`, ...) with its `will_siege`, `stuck` and `advice`.
  `set_war_stance_record_walk` still turns it off.
* **A stance the bridge did not read is no longer shown as the engine's.**
  A war row is `stance_source: "engine_stance_db"` only when the bridge says
  it read the stance (`stance_known` true). Otherwise the row is inferred
  from occupation (`attacker_not_sieging`, `offensive_sieging` or
  `defending`, with `stance_source: "inferred_from_occupation"` and
  `will_siege: null`) and carries:
  - a new key `stance_unread`, `null` when the engine stance was read, else
    a reason code naming the step that did not run or did not see:
    `query_war_stance_unserved`, `no_native_row`, `stance_ptr_unmapped`,
    `no_coordinator_record`, `record_walk_disabled`, `stance_db_unresolved`,
    `record_walk_not_run`, `walk_status_unreported`;
  - a limitation `{"code": "stance_unread", "reason_code": ...}`.

  Defender and ally wars always carry `no_native_row`: the bridge lists only
  wars the character leads as primary attacker. An attacker row at
  occupation 0 that is not the engine's word now also carries the
  `war_stalled_no_lever` limitation. The top-level `stance_source` is
  `engine_stance_db` only when at least one row is (it was whenever the
  bridge answered at all, even with no wars), and the `note` names
  `stance_unread`. All of this reaches `inspect(subject="war_stance")`,
  `observe`'s `war_stance_brief` (rows and `stalled`) and `plan_war`'s
  `any_stalled`.

## 2026.12.7 — 2026-11-10

For experiment cells: a seat can no longer claim a ruler the cell reserved
for another seat before that seat binds it.
The surface is still thirteen tools; `ck3agent run` is unchanged, and so is
everything outside an experiment cell.

* **A seat cannot claim a ruler the cell reserved for another seat.** In
  two cells of a pilot run the king's seat called
  `session(action="add")` on all five dukes' declared characters within its
  first minute; every duke's seat was then refused `owned_by_other` (one of
  them 43 times). An experiment cell now tells each seat's server which
  characters belong to the other seats: `CK3_RESERVED_FOR_OTHER_SEATS`, a
  comma-separated list of ids, with `CK3_SEAT_DECLARED_CHARACTER_ID`, the
  seat's own declared ruler, beside it. `session` `add` or `control` of a
  reserved character is refused `reserved_for_another_seat` by the server
  itself, before anything is sent to the bridge; it is not retried and does
  not fall through to `owned_by_other`. `release` is not a claim and is never
  refused this way, and the seat's own ruler, or an heir who is not another
  seat's declared ruler, still goes to the bridge as before. If the list is
  set but cannot be read (not a list of positive ids, no
  `CK3_SEAT_DECLARED_CHARACTER_ID` beside it, a
  `CK3_SEAT_DECLARED_CHARACTER_ID` that is not a positive id, or a list that
  reserves the seat's own declared ruler), every claim is refused `seat_reservations_unreadable`
  until the cell fixes it: a reservation the server cannot read is not
  treated as none. With `CK3_RESERVED_FOR_OTHER_SEATS` unset or empty,
  nothing changes. The `session`
  tool's description now says so, and `docs/reference/bridge-contract.md`
  lists both refusals.
* **The seat's lineage records write two new notes.** In a cell
  (`CK3_SEAT_LINEAGE_OUT`), a refused claim writes a `claim_refused` row
  naming the seat, the action, the character and the reason, so the record
  shows the attempt itself, not just its effect on the other seats. When the
  seat's lineage records a `bound` or `rebind` row whose ruler (the seat's
  primary ruler, the one the lineage follows) is another seat's declared
  ruler, it writes a `holds_reserved` row beside it. This covers the routes
  that are not a claim: its own ruler's succession, the bridge's heir
  handoff, or a continuation whose primary carried ruler is another seat's.
  It does not cover a reserved ruler a continuation carries as a second or
  later ruler, and it is written only while the seat's reservations can be
  read: with an unreadable list the note is skipped (claims are refused
  `seat_reservations_unreadable` instead). Neither note changes the seat's status, its ruler
  or its count of failed reads, and a resumed seat's server does not read
  them as lineage.
* **The cell summary dates each seat.** The research harness's cell summary
  now gives every seat its `started_utc`, `ended_utc` and `server_release`.
  In the pilot, two seats both listed the same character as held at their
  stop; they stopped 26 seconds apart and the first had released him before
  the second claimed him. The bridge never had two holders; the summary just
  could not show the order.

## 2026.12.6 — 2026-11-10

For experiment cells: a failed lineage read no longer records a seat that
holds its ruler as having lost it. The surface is still thirteen tools; `ck3agent run` is
unchanged.

* **A lineage read that fails is read again, up to three times in a row.**
  In an experiment cell a seat's server checks its ruler at every `observe`
  and writes its seat-lineage rows (`CK3_SEAT_LINEAGE_OUT`). Until now a read
  that failed ended the seat at once: a read of the bridge's sessions that
  timed out, lost its connection or came back without a session list ended it
  `seat_unbound` (`session_row_absent`), and a failed read of the ruler or the
  title ended it `character_read_refused`, `title_read_refused` or even
  `lost`. Two seats of a pilot cell were ended `session_row_absent` at
  867.10.22 and 10.23 while each held its ruler on the same bridge session to
  the end; the pilot kept no log that shows which reply their reads got.
  Now a read of the sessions, of the ruler (`query_character`) or of the
  title the lineage follows (`query_title_hierarchy`) that came back empty,
  timed out, lost its connection (`bridge_disconnected`), was not run in time
  (`game_thread_timeout`) or faulted (`memory_access_fault`, or a reply the
  bridge marks `degraded` that lacks what the step reads: the session list,
  the ruler's `alive`, the title's holder), or a step that raised, writes a
  non-terminal `read_failed` row, and the next `observe` reads again; the
  first step that reads again writes `reads_recovered`. A `degraded` reply
  that carries what the step reads is the bridge's answer and is decided on.
  The third such step in a row ends the seat `seat_unbound` with the reason
  `reads_failed_repeatedly`, dated at the first of them (`first_failed_utc`
  beside it); a resumed seat's new server continues the count from the rows.
  This counts from the seat's first `observe`, before it has a ruler too, so a
  seat that never gets a good read is recorded `seat_unbound`, not left
  unwitnessed. A seat whose own session is read and holds no ruler yet (a
  pre-bind that has not landed) is counted the same way, and three such reads
  in a row end it `seat_unbound` with the reason `never_bound`; it used to end
  `character_read_refused` at its second `observe`. A seat that reads its
  ruler after such steps is recorded `bound` as its first ruler; one whose
  rows are only such notes when the episode ends is graded unwitnessed, not
  bound. A read the
  bridge made and answered still decides at once: a session list without the
  seat's session is `session_row_absent`, a ruler or title read the bridge
  refused is `character_read_refused` or `title_read_refused`, a title the
  bridge reports not found is `lost`.

## 2026.12.5 — 2026-11-09

From the first pilot cell on 2026.12.4 (six seats, one game-year): a cell
seat whose claude was resumed lost its ruler for the rest of the year. One
fix for that, one line of the play protocol reworded, and three fixes the
smoke of the first draft found. The surface is still thirteen tools.

* **A resumed seat keeps its ruler: the new server takes over the old
  session.** In an experiment cell the bridge keeps a seat session's rulers
  when its connection drops, and refuses to release a control session's last
  ruler. When a cell seat's claude ended and `ck3agent run` resumed it, the
  run's release left the ruler with the old session, the resumed claude's
  new server got a fresh session, and every bind of the ruler was refused
  `owned_by_other` (the pilot's king, unbound from 867.10.01 to the end of
  the year). The run now hands the session over: it leaves the session
  record (`<stamp>.session.json`) in place, waits until the old session has
  no connection, and the new server presents that record's resume token at
  its first handshake and resumes the session, rulers and all. Each
  continuation's manifest row has a `handover` (the session, its state, its
  rulers, whether it could be resumed). A session that is still connected is
  never taken over: the new server gets a fresh session and the bind stays
  refused. On a resumed session the session's own primary ruler is bound
  first, so the pre-bind does not switch away from it. The session record
  carries `resumed` and `resume_status` when a handshake presented a token.
* **A run takes over the seat a dead earlier run left.** A `ck3agent run`
  killed before its teardown leaves its session record behind, and in a cell
  that session kept the seat's rulers against the next run. Each run now
  holds a lock (`<stamp>.run.lock`) while it runs; a new run in the same
  play directory takes over a left-behind session when its run's lock is
  free and the bridge shows it without a connection, and records it in the
  manifest (`adopted_session`). A run still going, a connected session, or a
  record from a run before this version is left alone.
* **The bridge log never carries a resume token.** The bridge logged every
  handshake line verbatim, and a handshake that resumes a session carries
  that session's resume token, the credential for its rulers. The logged
  line now replaces with `<redacted>` the whole value, of any type, of every
  key whose name (read after its escapes are decoded, in any case) is `auth`
  or names a token, secret, password, API key, authorization or credential,
  including a secret inside a string that holds JSON; the rest of the line is
  kept byte for byte. A line that is not valid JSON is cut before the first
  text that could be a secret. The same rule applies to the action lines the
  bridge logs.
* **A session `ck3agent run` cannot read is not "gone".** A
  `get_bridge_state` reply that carries no session list (a read fault, a
  refusal) was read as an empty registry: the run's teardown reported a
  pinned session "already closed, nothing held" while it held the ruler. The
  run now reads such a reply as unknown: it reads again until its wait is
  spent and then reports the read failed, and the hand-over, the take-over
  and the manifest's `held_after_teardown` never treat it as nothing held.
* **A killed `ck3agent run` takes its claude with it (Linux).** After
  `kill -9` to the run, its claude played on to the horizon with nothing to
  stop it. The claude a run starts now receives SIGTERM from the kernel when
  the run dies. If the kernel refuses that request, claude still starts and
  its stderr log says so; a Python without `ctypes` starts claude as before.
  The manifest records which (`claude_dies_with_run`).
* **The play protocol's stalled-war line names no cycle count.** "shows
  `attacker_not_sieging` ... for two cycles" now reads "cycle after cycle":
  an experiment does not tell a seat cycle counts.

## 2026.12.4 — 2026-11-08

From the first dry experiment cell on 2026.12.3 (three seats, two cycles of 90
game-days): what an experiment harness needs from the toolkit to run a cell of
several seats on one game clock. The cell's waits, stops, continuations and
speed refusal apply only when a harness sets the experiment variables. These
apply to every session: `search_characters` is read-only; every `ck3agent run`
sets `CK3_SESSION_ID` in claude's and the server's environment (the declared
value or `run-<stamp>`), writes the new manifest keys, and sends the run
prompt's rewritten END paragraph. The `bound` lineage row is written only when
a lineage sink is set. The surface is still thirteen tools.

* **In an experiment cell, `session(action="wait")` returns at the end of the
  cycle.** The cell's seats were taught `wait(seconds=60)`, and a seconds wait
  was not held to the registered cycle (`CK3_CYCLE_GAME_DAYS`), so a seat's
  cycle lasted however many game-days the clock ran in its seconds. With
  `CK3_EPISODE_HORIZON_START` set beside the cadence and the horizon end (an
  experiment harness sets all three), every wait now arms the bridge's pause
  alarm at the next cycle boundary, the start plus whole cycles, capped at the
  horizon end, and returns when the game date reaches it, whatever `seconds` or
  `game_days` asked. The reply says `stopped: "cycle_end"`, or `"horizon"` with
  `horizon_reached: true` at the episode's end, beside `game_days_advanced`,
  `cycle_index` and the `requested_seconds` it was given. All seats of a cell
  share one schedule, so their waits arm the same boundary; no seat's wait
  writes a closing pause. A start without the cadence or the horizon end is
  refused (`cell_schedule_unreadable`). When the game date cannot be read at
  the end of the wait, `game_days_advanced` and `cycle_index` are null, not 0,
  and so is `horizon_reached`, not false, unless the wait already read the
  date at the horizon (`stopped: "horizon"`), which stays true.
* **In an experiment cell, `ck3agent run` brings a seat back.** The dry
  cell's two dukes ended their sessions themselves after their first cycle
  (claude `end_turn` after a summary) while the game ran on, and the run
  recorded them as finished. With `CK3_EPISODE_HORIZON_START` set, a claude
  that exits before the run stops it is resumed (`claude --resume`, the same
  MCP config and flags) with one fixed text: "The game is still running and
  the run has not stopped you. Continue: observe, decide, act, wait." At most
  `--continuations` times (default 40); each is a row of the manifest's
  `continuations` (time, game date, claude's exit and its result, and the
  rulers carried). Past the cap the run exits 3 with `stop_reason:
  "continuations_exhausted"`; a claude whose transcript names no session to
  resume also exits 3, with `stop_reason: "continuation_without_session"`.
  The resumed claude's new server binds the rulers the seat held when its
  claude ended (`CK3_AGENT_CHARACTER_ID` is set to them, read from the old
  server's release record, else the session the run released, else the
  lineage sink's last row), not the declared one, which after a death and a
  rebind is the dead original (the bridge refuses that bind `character_dead`);
  and a server with a lineage sink starts from the sink's rows, so a resumed
  seat neither writes a second `bound` row nor forgets a rebind. A claude that
  ended right after the server's horizon reply is not resumed. Without the
  variable nothing is resumed. The run prompt's END paragraph now says
  which limits the run stops a session at, and that a cell's session is
  brought back.
* **In an experiment cell, `session(action="speed")` is refused
  (`speed_registered`).** The game speed is the experiment's registered
  condition; the seats share one clock, so a seat that changed it changed it
  for every seat. Nothing is written. Outside a cell the action is unchanged.
* **`ck3agent run` stops itself at the episode's horizon.** The dry cell's
  runs stopped on `--until-date`, six game-days past the horizon, because
  nothing capped a wait already running. The run now stops when its ck3
  server's `session` reply says `horizon_reached: true` (or refuses a wait
  with `horizon_reached`): `stop_reason: "horizon"`, exit code 0, and the
  manifest's `horizon_reached` and `horizon_end`. A seat's narration naming
  the horizon is not the server's reply and stops nothing.
* **`ck3agent run`'s manifest: `cycles_met` is null under `--cycles 0`, and a
  resumed run's cost is the sum.** `--cycles 0` asks for no cycle count, and
  `cycles_met` read true for every such run, including a session that ended
  after one cycle. It is now null when no count was asked. A run whose claude
  was resumed writes one result event per claude process; the manifest's
  `total_cost_usd` and `num_turns` are their sums (`result_events` says how
  many), not the last process's.
* **`ck3agent run` names the seat's session (`CK3_SESSION_ID`).** The server
  writes it into every seat-lineage row as `seat_session_id`, and `run` set
  none, so every row would have carried null. The run sets it to the value
  the environment declares, or else `run-<stamp>`, and records it in the
  manifest (`seat_session_id`, `seat_session_id_source`).
* **The seat-lineage record has a `bound` row.** A seat's lineage sink
  (`CK3_SEAT_LINEAGE_OUT`) got rows only at a death, so a seat whose ruler
  lived to the horizon had no sink, and the record read it `unwitnessed`
  rather than bound. The first step that holds a ruler now writes one
  `bound` row (the ruler, the title the lineage follows, the date).
* **`search_characters` is a read for every bridge session.** It was missing
  from the bridge's read-only list, so an observe session was refused
  `observe_mode` (the kingdom census, whose first read it is, had to open a
  control session), and for every session it counted as a mutation: a session
  whose decision budget was spent was refused it. It is now read-only: an
  observe session may call it, and a spent budget no longer gates it (the
  budget gates mutations only). Its scheduling class is unchanged.

## 2026.12.3 — 2026-11-07

The third round of toolkit fixes, from the smoke test of the 2026.12.2 draft.
`query_war_stance` reads each side's own stance: a war's primary attacker read
as the defender because the bridge kept one of the war's two AI records, the
one it walked last (the defender's, in 39 of 87 wars in a core dump).
`do(action_class="decisions")` no longer trips the bridge's fault net (three
faults on one five-candidate call): the failure reasons are read only for
decisions the game shows. `fabricate_targets` rows name the task version the
bridge evaluated, and a plain `ck3agent launch` waits for the main menu and
says when the game does not reach it. The surface is still thirteen tools.

* **CI: the macOS platform-layer job runs on `macos-15`.** GitHub retired the `macos-14` image on
  2026-11-07 ("The configuration 'macos-14-arm64-us-default' is not supported"), which cancelled the whole CI run
  before the release build; the job is not required and keeps `continue-on-error`.
* **`query_war_stance` reports the queried side's stance.** In the 2026.12.2
  smoke, Charles (12015), war 13's primary attacker with occupation 8 and a
  won siege, read `is_attacker: false, stance: defender_defensive`, and
  `inspect(subject="war_stance")` showed `side: attacker` beside the
  defender's stance. The game's AI keeps one coordinator record per war per
  side, and the bridge kept one per war, so whichever side's record it walked
  last decided the row (in the 1097 core dump, the defender's record comes
  last in 39 of 87 wars). The row's side now comes from the war's participant
  lists and its stance from that side's record. `inspect` also refuses to
  pair a side with a stance read for the other side: such a row carries the
  limitation `stance_side_mismatch` and an inferred stance.
* **`do(action_class="decisions")` no longer trips the bridge's fault net on
  decisions the game does not show.** A five-candidate call made the SIGSEGV
  safety net fire three times on the game thread (recovered; the game lived).
  The faults came from the pass that reads why a decision fails, which the
  bridge ran even for decisions whose `is_shown` was false; the game appears
  to build that explanation only for decisions it lists (a reading of the
  game's code, not yet confirmed in a live game). The pass now runs only for
  shown decisions, so a not-shown decision's row is `decision_not_shown` with
  no fault behind it, or `cannot_afford` when its cost is more than the ruler
  has. This is a change: before, a not-shown decision whose failure keys
  were read came back as `decision_unavailable` or `cannot_afford`, since the
  reader looked at the failures before the shown flag. Skip-listed decisions
  now say why they are skipped: `measured.skip_origin` is `seed` (one of the
  three decisions the bridge ships on the list, each of which froze the game
  in testing) or `learned` (one that froze it on this machine). The seed
  list's comments had two decisions' names swapped, and the names are now
  checked against the hashes.
* **`do(action_class="fabricate_targets")` rows name the task version.** With
  `args={"seat": "minister_justice"}` the bridge evaluates that seat's
  version of the task (`task_fabricate_claim_minister_of_rites`), and now
  each row carries it as `task_used`, as the `council_task` reply already
  did.
* **A plain `ck3agent launch` waits for the main menu and says when the game
  does not reach it.** Without `--continue` or `--save`, `launch` returned as
  soon as CK3 started, and on a headless host whose game never reached the
  menu it said nothing. It now waits as the other launches do (120 s, 600 s
  on the first start while the shader cache is built) for the game's log to
  show the main menu, prints `game at its main menu` (`game past its main
  menu` if it got further), and when the wait runs out prints `game not at
  its main menu` with the stage the log shows (for example "CK3 is still
  loading") and exits 3; a bridge that never answered says `bridge socket not
  up`, as before. `--no-wait` returns at once, as before.

## 2026.12.2 — 2026-11-04

A hotfix for `ck3agent launch`. In 2026.12.1, every `--save` and `--continue`
launch that got past loading the history crashed CK3 about 17 seconds later,
because the `continue_game.json` it wrote held only `title`; `launch` now
writes the three keys CK3 reads and repairs a file 2026.12.1 left behind. The
first launch in a new game directory waits up to 600 s, not 120 s, while CK3
builds its shader cache, and a wait that runs out says what the game's log
shows. A crashed game's CrashReporter no longer keeps the bridge socket
answering, and the refusal names the process that holds it. Nothing else
changed: the surface is still thirteen tools.

* **`ck3agent launch --save` and `--continue` no longer crash the game while
  it loads.** In 2026.12.1 every `--save` and `--continue` launch that got
  past "End loading of history" died about 17 seconds later, before the main
  menu (four of four in the smoke test; SIGSEGV at ck3+0x80a6090, address
  0x20). The cause was the `continue_game.json` that `launch` wrote into the
  game's new directory: it held only `title`. Before the menu, CK3 reads
  `title`, `desc` and `date` from that file and does not check that each one
  is there, so the missing `desc` was a read of address 0x20. The
  `-userdir=` switch was not the cause. The plain game binary with no bridge
  reaches the game with `-userdir=` and a complete file, and crashes at the
  same address with a `title`-only one. `launch` now writes all three keys:
  `title` and `desc` are the save's name, and `date` is the time it was
  staged. A plain launch (no `--continuelastsave`) was not run on 2026.12.1,
  so whether it crashed too is not known. If a game directory that 2026.12.1
  left behind has a `title`-only file, every `launch` (with or without
  `--continue`) adds the two missing keys and says `continue_game.json
  repaired`; `--save` writes the file whole.
* **The first launch in a new game directory waits long enough.** That game
  directory has no shader cache yet, and CK3 spends minutes building one
  (about 600 MB). The 120 s default ran out while the game was still
  compiling, and the warning told the user to click Continue. `--continue`
  and `--save` now wait up to 600 s when the shader cache is empty, and say
  so. When a wait runs out, the message comes from the game's own log: still
  loading (with the shader-cache reason on a first start), at the main menu
  (only then does it suggest a click on Continue), loading the save, or in
  game without the bridge reporting it active. Any other screen is named as
  the log names it. A log that cannot be read, or that was last written
  before this launch (a previous run's), is reported as such, never as
  "still loading".
* **A crashed game no longer leaves its bridge socket answering.** CK3's
  crash handler starts a CrashReporter, which inherited the socket's
  listening descriptor along with `LD_PRELOAD`. The socket kept accepting
  connections with no bridge behind it, so the next `launch` refused with "a
  bridge is already listening … another CK3 with the bridge is running" when
  no game was running. The bridge's descriptors are now close-on-exec. A
  process the bridge loads into that is not the game (`/proc/self/exe` is
  not `ck3`) closes any listener it holds on the bridge's socket path and
  serves nothing. The refusal now names each process holding the socket,
  with its pid and executable. For a process whose executable reads as
  something other than CK3, it says that a crashed game left it behind and
  to end it. A process whose executable cannot be read is called
  unidentified, and the refusal says to check what it is before ending it;
  and a running game whose binary was replaced on disk (`ck3 (deleted)`) is
  still called CK3.

## 2026.12.1 — 2026-11-05

Erratum: in this release `ck3agent launch --save` and `launch --continue`
crashed CK3 while it loaded (fixed in 2026.12.2); a plain `launch` was not
run. The date above is one day late. It was published on 2026-11-04.

The second round of toolkit fixes, from the smoke test of 2026.12.0 and the
November pilots' transcripts. `ck3agent launch` runs the game in a user-data
directory of its own and no longer writes into your saves.
`session(action="wait", game_days=N)` stops at N days: the bridge holds the
stop (an ask of 10 days came back 25). Every decision the engine judged
unavailable faulted inside the bridge, on an address 0xF00000 low in its
header; named decisions now come back as rows with the requirement they
failed, or a refusal naming them. A ministry's council seats take their own
version of a task, `query_war_stance` lists the wars of a ruler at war,
`declare_war`'s casus-belli refusal and 105 more bridge codes reach the seat
typed, and `query(domain="conquest_targets")` is 7,300 characters, not 38,500.
`doctor` checks only the play directory it is told and says when Claude Code
has not trusted it, `serve` starts with one line, and the linux tarball carries
the documentation. The surface is still thirteen tools.

* **`ck3agent launch` no longer writes into your own CK3 saves.** The game it
  started used your `~/.local/share/Paradox Interactive/Crusader Kings III/`,
  so its autosave rotation replaced your `save games/autosave.ck3` (the
  2026.12.0 smoke test found a new one after a run) and `--save` rewrote your
  `continue_game.json`. The game now runs with `-userdir=` pointing at its own
  directory, `~/.local/state/ck3agent/game/` (`$XDG_STATE_HOME`; `ck3agent
  config` prints it). `--save` copies the save there. The first `--continue`
  copies the save your CK3 played last, and later ones load the save the
  launched game played last. The game's saves, autosaves and logs stay in that
  directory. The files at the top of your directory (your settings) are
  copied once, and every folder in it is linked (your `mod/` folder among
  them) except the ones the game writes into: saves, logs, crashes, dumps,
  exceptions, caches and player data, which are the launched game's own.
* **`session(action="wait", game_days=N)` stops at N days.** The server could
  read the date only between its own sleeps, so the stop was where a sleep
  happened to end: in the 2026.12.0 smoke an ask of 10 days came back
  `game_days_advanced 25` from the first 15 s wait. The bridge now holds the
  stop: `unpause_game {"pause_at": "Y.M.D"}` arms a pause the game thread makes
  on the first day at or past the target (refused, unpausing nothing, for a
  date that is not after today), `get_tick_info` reports it as `pause_alarm`
  (`armed`, `fired` with `paused_at`, or `cleared` by a pause), and the wait
  reply says `stopped: "alarm"`, `wait_mode: "alarm"` and `alarm.paused_at`. A
  tick that moves the date past the target before the pause holds is reported
  as `overshoot_days`, never rounded away. Against a bridge without the alarm
  the old repeated waits run and the reply says `wait_mode: "polled"` with
  `alarm.state: "not_served"`; an arm the bridge did not confirm in time also
  falls back to polling, and says `alarm.state: "not_witnessed"`. Every end of
  the wait but the alarm's own pause writes a closing pause, which disarms the
  alarm (an engine pause, the call budget, an error included); a closing pause
  the bridge does not confirm is reported as `pause_unconfirmed: true`.
* **A named decision comes back as a row, or as a refusal naming it.**
  `do(action_class="decisions", candidates=[...])` answered `available []`,
  `unavailable []` for a decision the game has: the server asked the bridge
  for valid decisions only, which drops every decision that is not takeable,
  and a decision whose evaluation faulted came back without its name, so the
  reply could not match it. The server now asks for the annotated list; a
  decision the engine judged is a row in `available` or `unavailable` with the
  engine's reasons (`decision_unavailable` with the loc-key of the
  requirement it failed, `cannot_afford`, `decision_not_shown` when the game
  does not show it to this ruler); one whose evaluation the bridge had
  to abandon (a fault or hang it caught, or a decision on its skip list) is an
  `unavailable` row with `decision_eval_faulted`; and a name the game has no
  decision for is refused as `unknown_decision`, naming it (a warning of that
  code when other names did match). The bridge names every evaluated row from
  the decision's own key and lists the requested names it matched nothing for
  (`unmatched`).
* **A decision the engine judges unavailable no longer faults.** Every
  decision that failed a requirement faulted inside the bridge (the empty
  reply above): to read the requirement's name the bridge evaluates the
  failing block a second time into a diagnostic record, and it destroyed that
  record by calling an address in the middle of an unrelated game function
  (the offset in the bridge's header was 0xF00000 low, 0x04a5b060 for
  0x0595b060: 0x1000000 subtracted from the disassembler's address where the
  image base is 0x100000), which crashed. The bridge now calls the destructor
  the game's own evaluator calls, and checks that at runtime against the
  game's code. A decision the engine refuses is an `unavailable` row with the
  requirement it failed. When the check disagrees the bridge skips the
  requirement's name rather than call an unchecked address, and the row says
  so: `decision_reason_unwitnessed` (the game said no; which requirement failed
  was not read). The rejection reasons of interactions and laws went through
  the same address and are read the same way now.
* **A council task can be given to the seat that has it for your government.**
  A ruler with ministries fabricates claims through the minister of justice
  (the game's `task_fabricate_claim_minister_of_rites`, a copy of the court
  chaplain's `task_fabricate_claim`), but `do(intent="council_task",
  args={"seat": "minister_justice", "task": "task_fabricate_claim", ...})` was
  refused with `seat_cannot_take_task` and `task_seat
  councillor_court_chaplain` before the game was asked. A seat now picks that
  seat's version of the task; the reply names it in `task_used`, and a seat
  that has no version of the task is refused with every seat that does
  (`task_seats`). `do(action_class="fabricate_targets")` takes the same
  `seat` in `args` and its rows' `do_call` name the seat the bridge
  evaluated. With no `seat` the court chaplain's task is tried first, so a
  ruler with ministries passes `args={"seat": "minister_justice"}`; when the
  chaplain's version finds nothing, the `fabricate_targets_empty` warning
  lists the task's other seats (`task_seats`).
* **`query_war_stance` lists the wars of a ruler at war.** A call without
  `war_id` read the missing argument as the war id 0xFFFFFFFF and dropped every
  war, so a ruler fighting war 13 got `wars []` and `war_count 0` while the
  record walk had matched 30 records, and `inspect(subject="war_stance")` fell
  back to the occupation guess. Without `war_id` (or with 0) the reply lists
  every war the character leads as primary attacker; a `war_id` still scopes
  it to that war. `query_war_score_breakdown` without `war_id` now answers
  every cached war, as documented, instead of nothing.
* **`declare_war` refused by the game's casus-belli check says so.** The
  refusal reached the seat as `unmapped_refusal` with the bridge's raw
  `engine_cb_validity_check_failed`. It is now `cb_invalid_for_target`, not
  retryable, with `fix` naming `cbs(target_id=…)` and `plan_war()`, the two
  calls that list what can be declared. The same sweep typed every other code
  the bridge's write paths emit (105 more, from a census of the bridge
  source): missing and invalid arguments, names nothing matches, missing or
  dead characters, `engine_unreachable` (the bridge could not make the call;
  nothing reached the game), `write_not_applied` (the game ran it and the
  readback shows no change) and `bridge_fault`. The census walks the five
  source files of the write paths a tool sends; in them only the operator
  verbs no tool sends, and the AI-pipeline declare's own "unknown" rejection,
  stay unmapped. Codes emitted elsewhere are not covered by it, among them
  the research-only budget guard's `budget_exhausted` and
  `budget_config_pinned`.
* **`query(domain="conquest_targets")` is 7,300 characters, not 38,500.**
  The compact reply still listed 25 counties at ~1,500 characters each (two
  declare blocks per row) plus the bridge's long note, so a large realm's
  answer was 38,481 characters. The default now lists 10 counties outside your
  realm, conquerable first, each with the holder, the holder's realm, its
  de-jure duchy and kingdom, adjacency, the verdict and its source, the gates
  that are set and one `declare_args` that `declare_war` takes as it stands;
  the engine's CB list and the other declare options are counts.
  `detail="full"` gives the rows with every field the bridge writes on them
  (the kin, war and liege flags included; a superset of the default row) and
  the note, `max_results`
  lists more, `verbose=true` is the bridge's reply unchanged. Any other
  `detail` is refused as `invalid_argument`.
* **`query(domain="character")` names the character.** The first read of a
  character in a session came back with no `name` (only the game's
  `name_key` and `name_pending`), because the bridge localizes names on the
  game thread after the read that asks for them. The query now reads again
  while that happens (about 1.5 s at most); a name still pending after that is
  `name: null` beside `name_pending: true` and a `name_note` saying to read
  again, never a bare null.
* **`discover(what="scheme_targets")` answers for a character you have not
  bound.** It is a read (the game's own start check; nothing is started), but
  the bridge guarded it as a write, so a call with an explicit `character_id`
  this session had not claimed was refused `actor_not_claimed`. The check now
  passes the guard as every read does; starting and cancelling a scheme still
  need the character bound.
* **Releasing your only ruler says what to do instead.** The refusal
  `last_claim_of_control_session` explained what was refused and nothing else.
  Its `blocked.fix` now names the two ways out. The ruler is freed when the
  session ends, and the game's AI plays it on with your last posture. To switch,
  call `session(action="control", character_id=<other>)`, which moves the seat
  in one call.
* **A run's teardown leaves another run's ruler alone.** With two
  `ck3agent run`s on one game, the second to finish tried to release the
  first one's ruler and warned that it could not (`not_owned_by_session`).
  The teardown now checks and releases only the rulers its own server's
  session holds (the session the record names); a ruler another session holds
  is named in the line and left alone, without a warning. When the session
  record is missing or cannot be read, nothing can be attributed: the teardown
  releases nothing and warns `session_record_missing`, listing the rulers it
  saw bound as unattributed.
* **`ck3agent doctor` checks only the play directory it is told.** With no
  setup record it read `~/ck3agent-play/.mcp.json` anyway, so a directory left
  by another install failed the check (rc 1 on a clean `XDG_CONFIG_HOME`, in
  the smoke test of 2026.12.0). Doctor now reads `--play-dir` or the directory `setup`
  recorded in the config. With neither it warns `play directory not set up: no
  --play-dir and none recorded in <config>` and reads nothing.
* **Setup and doctor say when Claude Code has not trusted the play
  directory.** Until you accept Claude Code's trust dialog once, it ignores the
  allow-list `setup` wrote. Every run in a fresh play directory printed
  `Ignoring 1 permissions.allow entry ... this workspace has not been trusted`,
  and nothing in ck3agent explained the line. `ck3agent run` was not affected,
  because it passes `--allowedTools mcp__ck3` itself. `setup` now ends with the
  one-time step. `doctor` reads Claude Code's record in `~/.claude.json` (read
  only) and reports `Claude Code has not trusted the play directory` until the
  dialog is accepted. Troubleshooting quotes the stderr line.
* **`ck3agent serve` starts with one line, not the 64-line handshake.** The
  server printed the bridge's whole handshake reply as indented JSON to
  stderr at every start. It now logs one line (`bridge_version`,
  `build_git_sha`, `build_id_status`, `session_id`) and keeps the reply in
  `handshake_ack.json` in the instance's telemetry directory, which the line
  names (with `-v`, the reply is also logged at DEBUG). The kept reply and the
  DEBUG line leave out the bridge's `resume_token`: the file is for reading
  the build, not for resuming a session. The file is written owner-only
  (0600; a directory it creates is 0700, an existing one keeps its mode) and never
  through a symlink.
* **The linux tarball carries the documentation.** `docs/` (getting started,
  troubleshooting, `reference/tools.md`, `reference/files-and-paths.md`) was
  in the sdist only, and the tarball's `README.txt` pointed at a URL. The
  tarball now has the whole `docs/` directory: about 92 KB compressed, 241 KB
  unpacked. `README.txt` names the local files. `release.yml` checks the
  tarball's listing for each of them before it publishes.

## 2026.12.0 — 2026-11-04

The toolkit fixes from the review of the November pilots' transcripts. The
agents read the refusals right; the tools lost them the ground. The first
`set_phase` holds (12 of 32 seats had it undone at the first tick), the action
log says what an action did (42 rows said a war failed; 39 of those wars were
declared), an argument a tool does not take is refused instead of dropped (174
`pass_turn(character_id=...)` calls), `plan_war`'s claim rows can be declared,
replies that ran to 186,000 characters are bounded, and a surrender says it is
your defeat (six of twelve seats paid for one, believing it free).
`session(action="wait")` can wait in game-days, `ck3agent run` counts the
cycles and ends the run itself, and the shared board is gone. The surface is
still thirteen tools.

* **`session(action="wait")` can wait in game-days.** A new optional
  `game_days` argument: the server repeats bounded waits (each at most
  `seconds`, ≤ 110 s, the next one planned from the clock rate it measured)
  until the game date has advanced that many days since the call began, and
  reports `game_days_advanced`, `calls` and why it stopped. Seconds were the
  only unit, and the clock's rate depends on the host and the number of
  sessions, so "wait 60 days" at the prompt overshot by a factor of ~2.7 in a
  six-seat run. Without `game_days` the tool behaves exactly as before. When
  `CK3_CYCLE_GAME_DAYS` names a registered cadence, an ask above it is refused
  (`cycle_game_days_over_cadence`), never clamped. When `CK3_EPISODE_HORIZON_END`
  names a registered horizon, a wait made at or past that game date is refused
  (`horizon_reached`); without the variable nothing changes. The 13-tool surface
  is unchanged.
* **The shared board is removed.** The two arm-gated tools `board_post` and
  `board_read`, the bridge's `board_post` / `board_read` / `board_admin` verbs
  and the `CK3_BOARD_*` variables are gone. They were an experiment channel; no
  public run used them. The server registers the same 13 tools in every
  process.
* **The first `set_phase` holds, and the reply says whether it does.** In the
  first test runs the first `set_phase` of 12 of 32 seats was undone at the
  first tick: the bridge first saw the world after unpause and set aside, as
  a reload, the personality written while paused at the start date. The
  bridge now samples the world at every drain, before any queued action.
  `set_phase` answers `result.lock_state` (`live`, `quarantined` or `none`,
  the bridge's word) and `world_epoch`, and warns
  `personality_lock_not_live` when the write is not held; `observe` carries
  the same field under `posture.genome.lock_state`.
* **The action log records what an action did.** Each row of
  `action_log.jsonl` was the enqueue receipt (`success: false`, `queued`), so
  42 pilot rows said a war failed while the executor declared 39 of them.
  Every row now carries `verdict` (`applied`, `rejected`, `pending`,
  `unknown`) and, apart from it, `poll`; when the game thread runs the action
  the bridge writes an `outcome` row with the same `dispatch_id`, the verdict
  and the result (the war id for a war).
* **An argument a tool does not take is refused.** A call with an unknown key
  ran as if the key were absent: `pass_turn(character_id=...)` 174 times in
  the pilots, `steer` with the axes flat once. Every tool now answers
  `ok: false`, gate `unknown_argument`, naming the keys in
  `fix.rejected_fields` and the ones it takes in `fix.accepted_fields`, and
  runs nothing. `pass_turn` takes `character_id` and names the ruler who
  passed.
* **`declare_war` takes the `claimant_id: null` that `plan_war` writes.** A
  `plan_war` row's fields passed one by one were refused at the tool's door
  (`Input should be a valid integer`); null is no claimant.
* **`plan_war`'s claim rows are declarable.** A claim row kept the county's
  title where the game accepts the war on the title of the pressed claim, so
  `declare_war` refused it (`claim_not_pressed`); and the claim and
  independence rows of the war-target list were dropped (the bridge writes
  `target_char_id`, the server read `target_id`). A claim row now carries the
  claimed title and the claimant, and a row whose casus belli needs a
  claimant is offered only on a pressed claim, also when no casus belli
  options could be read; any other is listed in `excluded`
  (`claim_not_pressed`). A goal-scoped plan no longer counts your own
  vassals' counties as targets; `empty_reason.measured.own_realm_counties`
  says how many it set aside.
* **`plan_war`'s empty reply says how long it has been empty.**
  `empty_reason` carries `unchanged_since` (the game date this ruler first
  got the same reason for the same call) and `times`. A row pruned past the
  probe cap said "re-plan to surface", which the same call never did; it now
  says raise `max_results`, with the value in `fix.max_results`.
* **Big replies are bounded.** `do(action_class="decisions")` evaluates the
  names in `candidates` and returns only those: the bridge read the list at
  the wrong level, so every call returned every decision, unevaluated (about
  186,000 characters even for one name). `inspect(subject="war_score")`
  without `war_id` returns your wars, not every war in the game (394 rows,
  118,000 characters). `query(domain="conquest_targets")` returns 25 rows,
  the conquerable first, with the counts (`max_results` or `verbose` for
  more).
* **A surrender says it is your defeat.** Six of twelve budget-pilot seats
  surrendered right after a refused white peace, believing it free; each paid
  15 to 350 prestige and up to 590 gold. `do(intent="make_peace",
  terms="surrender")` warns `surrender_is_your_defeat`, and
  `inspect(subject="war_stance")`'s `exit_lever` carries `surrender_cost`.
  The protocol says the same.
* **A stalled attack says no lever moves it.** The game's AI besieges only
  when it is above 1.33× the enemy's strength, an engine constant that no
  personality axis or phase reaches. A war row inferred `attacker_not_sieging`
  carries the limitation `war_stalled_no_lever`, naming the one lever there
  is: call allies into the war, or leave it open.
* **Claims are fabricated through the council.** The `generate_claim` scheme
  targets a title: `discover(what="scheme_targets")` and a start through
  `do(intent="scheme")` both refuse it `scheme_type_targets_a_title` (the
  start said `engine_refused_can_start_scheme`), and the refusal and the
  protocol point to `do(action_class="fabricate_targets")`. The "pass `target_id=0`" hint now
  appears only for interactions aimed at yourself. An empty
  `fabricate_targets` list warns `fabricate_targets_empty`, naming the seat it
  used (the court chaplain) and the bridge's counts; a ruler whose realm
  seats the task on a ministry is not looked up yet.
* **`war_duration_unavailable` is gone.** It fired on 391 of 392 observes, at
  peace too, and was about the succession estimate;
  `succession_eta_basis` beside the estimate says what it is based on.
* **The live-state hook shows your own ruler.** With two sessions on one
  game the hook showed the ruler of the session that claimed first to both
  seats. It now reads its seat from the session record `ck3agent run` hands
  it (`CK3_SESSION_OUT`), else the declared `CK3_AGENT_CHARACTER_ID`; when
  two or more control sessions are attached and neither says which is yours,
  it shows no ruler and points to `session(action="whoami")`, also while your
  own session holds nothing yet. A seat given a session record that names no
  attached session (`CK3_SESSION_OUT` set under `ck3agent run`, and its
  server's handshake not landed or its connect failed) is shown no ruler
  either, since the one attached session may be another seat's. Only with
  one seat on the game, and no such record, does it show the bridge's ruler.
  One case the hook cannot tell apart: an interactive seat (no record, since
  `ck3agent setup` writes none) whose own server has not attached yet, or
  whose connect failed, while one other seat is attached, looks like the only
  seat and is shown the bridge's ruler, which is that other seat's. Set
  `CK3_AGENT_CHARACTER_ID` to close it. Another session's rulers are
  otherwise named as held by another session, never as yours.
* **The ck3 tools load at the start.** `ck3agent setup` and `ck3agent run`
  register the server with `alwaysLoad: true`, so Claude Code lists the tools
  up front instead of deferring them behind a tool search.
* **`ck3agent run` counts the cycles and ends the run itself.** The run
  prompt said "BUDGET: N cycles" and the model counted; it now names no count.
  The run counts cycles from the transcript as they happen and stops the
  session when `--cycles` are done (stop reason `cycles`, exit code 0), as it
  does at `--until-date` and `--wall-seconds`; `--cycles 0` turns the cycle
  stop off. A `--prompt-file` may still use `{{cycles}}`. The manifest gains
  `cycles_completed` and `cycles_met`, `observe_calls`, `wait_groups`,
  `ended_on` (the last ck3 call), and `total_cost_usd_source` (Claude Code's
  estimate, not a bill). The cycle's definition is written beside the count.
  The run keeps one live counter, fed from the transcript as it is written;
  any stop (`--cycles`, `--until-date`, `--wall-seconds`) freezes it when it
  fires, so `cycles_completed` is the counter as it stood at the stop.
  `cycles_recounted` re-reads the finished transcript with the same
  definition, as a cross-check: it can exceed `cycles_completed` by a call
  that was in flight when the stop fired.
* **The boot text no longer mentions a run-prompt budget.** `protocol/CLAUDE.md`
  says "Play in cycles until the human stops you"; a harness that bounds a run
  ends the session itself rather than asking the model to count.

## 2026.11.0 — 2026-11-02

The toolkit, redone from a player's seat. Thirteen tools (`intent` is folded
into `do`), every one of them working with the game paused or running, and
every reply the same shape: a write answers `applied`, `pending` or `rejected`
with the gate that refused it and the game's own words. Wars can be finished
(`make_peace`, `call_ally`, a war score that reads), every scheme the game has
is listed with the game's reasons, factions work from both seats, and
`succession_plan` lists the levers you actually have. Actions that never did
what they said are gone rather than documented: the event answers, the
subsystem toggles, `muster`, `force_offensive_stance`, `evaluate_trigger`. The
Windows port is documented as it stands. The bridge and tool changes below were
proven against a live game before they were merged.

* **The bundle installs with current pipx offline.** pipx 1.8 refreshes the
  pip inside its shared venv with the install's own pip arguments, so the
  offline install (`--no-index`, the bundled wheels) failed there and
  `install.sh` fell back to its private venv with a warning. The bundle now
  carries a pip wheel; pipx stays the install method where it is present.
* **A bridge fault is a typed refusal.** When the bridge's own read or call
  into the game faults (`memory_access_fault`) or the game thread does not
  finish it in time (`game_thread_timeout`), a write answers `rejected` with
  gate `bridge_fault`, not retryable: the effect may have landed, so read it
  back before sending it again. It is never `unmapped_refusal` and never a
  retryable `drain_timeout`. A read the game thread did not run in time is
  `read_timeout` (retryable), and a read that faulted is a refusal, no longer
  `ok: true` with the fault as its data. A write in flight when the connection
  dropped is `write_unconfirmed` (it may have landed), and a `session(wait)`
  whose closing pause is refused answers `rejected`, not `pending`.
* **A court can be read by sex in one call.** Each `query(domain="characters")`
  row now carries `is_female` and `dynasty_name` (when the character has one),
  and `female=true|false` keeps one sex (unset keeps both; the reply echoes
  it): `query(domain="characters", court_of=<your ruler>, female=true)` lists
  the women at your court.
* **A dynasty has one name everywhere.** The observation feed's ruler
  `dynasty_name` was read from the dynasty's id field and came back empty or
  garbled; it now reads the dynasty's name. Every dynasty name (the feed, the
  character rows, `inspect(subject="dynasty")`) follows the game's rule: the
  custom name when the dynasty has one set, else its name. When the name cannot
  be read, `inspect(subject="dynasty")` leaves `name` out instead of saying
  "(unknown)".
* **A refused write says the game's own words in its reply.** `blocked.detail`
  now carries the game's sentence for the refusal ("You cannot grant your
  Realm Capital", "Missing prestige 960"), read from the drained write's
  record or the reply's own engine text; `"the game gives no detail"` when the
  game was asked and said nothing. `blocked.summary` stays the typed code. The
  pending list now also says the reply's words for a refusal the bridge stored
  without them. A refusal whose only line is a price is
  `insufficient_prestige` / `insufficient_gold` / `insufficient_piety` (or
  `cannot_afford`), retryable, not "never".
* **`plan_war`'s `empty_reason` names what excluded the rows.** It is the
  commonest `excluded` gate with its count (every gate in
  `measured.by_gate`), and `fix` names the parameter that would admit them
  (`exclude_straddling=False` with `confirm_straddle` on the declare;
  `ally_free_only=False`). It said "allied/truced" whatever the rows said. A
  holder who is someone's vassal is `target_not_independent`.
* **A grant reads its new holder before answering.** A title grant that
  already shows on the pass answers `applied` with the holder in
  `result.readback`; otherwise it is pending over days as before.
* **The pending block drops its duplicates.** `pending.intent_id` is the
  token (`do(intent="cancel_pending", args={"request_id": <intent_id>})`);
  `poll_token` is gone where it repeated it, and so is `timeout_s`.
* **`set_phase("expansion")` no longer arms the off-goal war veto, and a
  switch needs its value.** The veto is an experimental hook no live run has
  validated, and it was armed process-wide by the default posture; it is now
  armed only by `session(action="set_experimental", flag="off_goal_veto",
  enabled=true)`, and `set_phase`'s result no longer carries
  `offgoal_veto_armed` / `offgoal_veto_scope`. `session(action="set_experimental")`
  requires `enabled`: a call without it is `invalid_argument` and writes
  nothing (it used to arm the flag).
* **One answer when no ruler is bound.** `steer`, and `query` and `discover`
  for anything about your ruler, now answer like `observe` and the other
  actor tools: `ok: false`, `blocked.gate: "no_ruler_bound"` with its `next`.
  They answered `unmapped_refusal`, `errors[]`, or `ok: true` with a list. A
  read that names no character (the character browser, the trait and
  interaction lists) still answers.
* **`steer` answers like every other write.** A personality write now answers
  `verdict: "applied"` when the axes read back as written (in
  `result.readback`), `rejected` with `blocked` when the bridge refused it or
  the axes do not read back as written, and `pending` when the bridge did not
  confirm it and a read does not show it. A bad key or value is `rejected`
  `invalid_argument` with nothing sent. `dominance` is no longer an axis: the
  engine keeps no byte for it and the game's AI never read it, so `steer` and
  `set_phase` refuse it and no phase writes it (9 axes, honor through
  sociability).
* **Every tool works paused or running, and only `session` touches the
  clock.** `query`, `discover` and `steer` no longer answer `game_unpaused`
  while the game runs, and no write refuses `game_paused`: the bridge serves
  every read and runs every write on the game's command thread whether the
  game is paused or not, and a write lands within one pass (a few
  milliseconds) on a paused game. No tool pauses, unpauses or changes the
  speed any more; `session(action="pause"|"unpause"|"speed"|"wait")` are the
  only calls that do, and a loaded save stays paused until you unpause it. A
  write has two waits. The first is the bridge running it: a queued write
  runs on the game's next pass, paused or running, and the server reads its
  verdict back within about three seconds. A refusal answers `rejected` with
  the game's reason. A write whose effect shows on that pass is read back and
  answers `applied` with the game paused or running: `declare_war` (the war
  listed), a title (held), a title law (the title's laws), a gift (the
  sender's gold), surrender (the war ended), a call to arms (the ally on the
  war; an ally still absent declined). If the verdict cannot be read in time
  the reply is `verdict: "pending"` with `pending.drains_when: "next_pass"` and
  the `pending.intent_id` to read it by. A write the bridge ran whose effect the
  server could not read (running, a gift's two purses read across a daily tick)
  is pending with `drains_when: "readback"` and `result.sent: true`, never
  `rejected`: read it before sending it again. The second wait is game days: an
  answer that comes on a later day (a white peace or enforced demands, a
  vassal answering a revocation, an alliance proposal, an own-court interaction
  the game applies on its daily tick), and every interaction the server cannot
  read back yet, answers `verdict: "pending"` with
  `pending.drains_when: "clock_advances"`,
  `pending.game_date_at_submit`, `pending.expected_change` (what to check) and
  `result.sent: true`; one with a readback (a peace offer's war-end ledger) is
  first polled for a few seconds while the game runs. Nothing runs the clock to
  get it. The `game` block of every reply says whether
  the game was paused when the tool was called, read from the bridge at the
  call (it said paused on a running game before). The gates `requires_paused`
  and `requires_unpaused` are gone, and the live-state hook reads the ruler,
  realm and succession sections in both states. With no client connected and
  its auto-respond setting on, the bridge unpauses only when the game's own event pause
  stopped it, never a pause you set. `CK3_SUPPRESS_AUTO_UNPAUSE` is gone with
  the behaviour it suppressed. Every clock write the bridge makes is counted by
  who asked, in `get_tick_info`'s `clock_writes`. The Windows bridge does the
  same: it runs its game-thread work after every pass of the game's command
  thread too (an atomic swap of the thread's loop pointer, the work under the
  game's own lock), so a save loaded paused goes ACTIVE with no tick and reads
  and writes answer while paused (verified under Proton; real Windows is the P7
  run's re-check).

* **Factions, from both seats.** `do(action_class="factions_to_join")` now asks
  the game. It lists every faction type a vassal may create against his liege,
  every faction aimed at that liege he may join, the ones he is in (may he
  leave), and for one he leads whether he may press its demand now. A refused
  row carries the game's own reason in `blocked.summary` (gate
  `faction_refused`, e.g. "Emir Abu Abdallah already has the lowest possible
  Crown Authority"), and a member's row shows the demand path: discontent
  against the threshold, whether its AI leader may press the demand and his
  chance each month (`ultimatum`: `ai_may_press` with `ai_press_chance_pct`, or
  `none`), the cooldown after a pressed demand, and the war the faction declares
  if refused. A row the bridge refuses before asking the game (you do not lead
  the faction, are not in it, or have no liege) has gate `faction_precondition`
  and says so in plain words ("you are not this faction's leader; the leader is"
  and his name and id); `faction_refused` is the game's refusal alone, its
  summary the game's sentence. It used to list the same
  three faction types for anyone. `do(intent="faction")` takes `action`:
  `create` (with `faction_type`, which no longer defaults to
  `independence_faction`), `join`, `leave` and `press_demand` (with
  `faction_id`), and `respond_demand` (with `response`: `accept`, `refuse` or
  `ai`), a liege's standing answer to a faction's ultimatum, which the bridge
  (new action `set_faction_demand_response`) gives with the game's own accept
  or refuse option. `target_id` is gone:
  `liege_id` names the liege, the vassal's own when omitted. Every write is
  checked by a second read before the reply says `applied`: a create, join or
  leave by the member list of the liege the faction is aimed at, a pressed
  demand by the leader's own faction read a tick later (the game must now refuse
  a second press). A write the second read cannot see comes back pending
  (`drains_when: "readback"`, `pending.raw_reason: "write_unconfirmed"`,
  `result.sent: true`), never `applied` and never `rejected`; one the second
  read shows did not take
  (still a member after a leave, the demand still pressable after a press, a
  standing answer that reads back different) is `faction_write_not_applied`. A
  call into the game's faction code that faulted is contained and answers
  `faction_bridge_fault` (read `factions_to_join` before assuming nothing
  changed), and a faction read made while another one runs answers
  `faction_query_busy` (read again).
  `inspect(subject="factions")` lists each faction aimed at the ruler with its
  leader and members by name and soldiers, discontent, power, threat and war,
  the ruler's standing answer and every demand answered for him (with what the
  AI would have chosen), and for each leader a gift and imprisonment (judged by
  the game's own send check, with the leader's answer) as ready `do` calls, and
  the council seats he already holds.
  It offers no council seat as a lever: `do(intent="appoint")` swaps the seats
  of two sitting councillors, so it cannot seat a leader who has none.
  Creating, joining, leaving and pressing are no longer off by default.
  The game keeps no ultimatum countdown, so no faction read gives one any more:
  `query_faction_threat` loses `days_to_ultimatum` and `ultimatum_delay_days`
  (a game define, 90 days), and `query_factions` loses
  `ultimatum_fire_date_raw` (the date the faction's tick writes 91 days ahead
  every month, never reached); every faction had read as counting down to
  0001.01.01. Every faction read now gives one demand state, classified in one
  place in the bridge: `ultimatum` is `ai_may_press` (with
  `ai_press_chance_pct`) when the faction's last monthly tick armed the demand
  (discontent full and the game's press check passing at that tick), no cooldown
  has started since, and the AI leader's chance to press at the next tick is
  above 0; otherwise `none`; `demand_cooldown_until` while a cooldown runs.
  `query_faction_threat`'s `can_fire_ultimatum` and `ultimatum_active` are that
  same `ai_may_press` (not a fresh call to the game's press check, which the
  vassal's row gives as `can_press_now`), and its `ultimatum_progress_pct` is the
  chance, 0 when `none`; `query_factions` gives `ultimatum` in place of the raw
  `ultimatum_progress_pct`. The turn banner shows the AI leader's monthly chance
  to press instead of days.
* **`do(action_class="imprison_targets")` rows are the game's own verdict.** Each
  target is judged by the send's own checks, run without sending
  (`query_interaction_check`, new): the game's validity with its sentence, the
  send conditions, the recipient's refusal and the cost. A row is offered only
  when the game would take the send and the target's answer is not "no". It
  carries the game's `answer` (`yes`, `maybe`) and `accept_score`, the target's
  acceptance score, and `chance_pct` only for a "maybe" whose score lies between
  0 and 100, the one chance the game defines; a "yes" is not a certainty and
  carries no percentage. The answer is the target's at check time: the game asks
  him again when the send arrives and he answers 4 to 9 days later on his state
  then (gifts, opinion and the days between move it; a target who read "yes" at
  239 can stay free), so check just before sending:
  `candidates=[<id>]` checks the targets you name (default: your first 40
  vassals). Imprisonment is the target's answer: a landed vassal who refuses
  rebels, a baron flees. Your rulers are AI-run in the game's eyes (the bridge
  plays an observer game, so no ruler is the player), so the game takes an
  imprisonment from them with grounds against the target or while they are
  involved in a struggle (a ruler in a struggle can jail a vassal he has no
  grounds against), never on the player's rule. Refused rows carry the gate a
  direct send of the same refusal gets, with the game's words:
  `interaction_invalid` (the game's validity or send conditions),
  `recipient_refuses` (the target would say no), `cannot_afford` (the cost).
  Grounds alone were not enough: targets with grounds were sent and the game
  declined them. Every row still lists the grounds (`grounds`, from
  `query_imprisonment_reasons`) and the check (`game_check`); offered rows come
  highest score first. It is the same row a faction leader's imprison lever shows
  in `inspect(subject="factions")`, and `discover(what="targets",
  action="imprison")` now gives these rows too (it listed vassals with their
  grounds alone); `discover(what="imprisonment")` is the grounds read, not the
  game's verdict. `inspect(subject="character")` gives `imprisoner_id`, the
  engine's link to who holds a prisoner.
* **A faction at war reads at war, with the war's id.** The faction rows read
  a byte that is not the war (it marks a faction a player is concerned in), so a
  faction at war, whose press the game refused "while the faction is in war",
  read `at_war: false`. Every faction read (`inspect(subject="factions")`,
  `do(action_class="factions_to_join")`'s demand rows, `query_factions`,
  `query_faction_threat`) now resolves the faction's war as the game does, and
  gives `war_id` while at war. `query_factions` loses `war_inactive` and
  `should_press_demand` (a misread byte).
* **Faction refusals are said in words.** Every refusal the bridge's faction
  code can give (already a member, no such faction type, a dead actor, ...)
  carries a sentence, never the code. `query_factions` no longer prints
  `cooldown_date_raw` beside `demand_cooldown_until` (one reading of the cooldown).
* **The live-state block says what you can act on.** Each war is its game name,
  your side and who the score favours (`Sons of Loðbrók Invasion of Lothian · you
  are the defender · war score 0 (even)`); it was `vs <id>` with the ruler's own id
  and a tied score called winning. With no ruler bound the block is the date and
  how to bind one, and nothing is read for "character 0". A dead ruler's alert
  names `session(action="whoami")` (the bridge binds the heir), not a retired
  call. The per-turn "do NOT rabbit-hole on marriage" and "no cheats / no console"
  lines are gone, and so are the council-vacancy, own-scheme-count, crown-authority
  and disloyal-vassal lines: the block reads the ruler, wars, heir (or **none**),
  whether the next succession partitions, the strongest faction against you with
  its ultimatum, and the plots against you. While the game runs it no longer
  prints a row of `?` for the profile it could not read.
* **`observe`'s war rows say only what they read.** A row in `wars.active`
  carries the war's id, name, your side, its leaders and, when the game has
  computed it, `war_score` with `favored_side` (`war_score_stale` when the
  computed score is old). It used to fill in `war_score_source`, `battle_score`,
  `ticking_score` and `sieges_in_progress` as None or 0 although this read never
  reads them, and so flagged every war at a score of 0 as "do not read 0 as
  parity". Each row now says in `breakdown` that those are read by
  `query(domain="wars")`.
* **`observe(depth="full")`'s AI evaluations name your side of each war.** A row
  in `evaluations.active_wars` is now the row `query(domain="wars")` gives for
  that war: it gains `is_attacker`, `cb_family`, `target_titles` and `sieges`,
  and loses `our_role` and `score_breakdown`. Its `my_side` could name the wrong
  side (on the 867 save it called the defender of the Sons of Loðbrók invasion
  of Northumberland its attacker); it now says what every other war read says.
* **A paused read sees what the game created since its last memory refresh.**
  The bridge refreshes its map of readable memory only every 100 game days, so
  a read made while the game is paused could miss anything created in up to
  ~100 days before the pause: a scheme you had just started was listed among
  your own schemes but not in its target's
  `inspect(subject="incoming_schemes")`. The bridge now refreshes that map once
  when the game pauses, before the first such read.
* **`ck3agent setup` no longer allows `Bash(sleep:*)`.** Time passes with
  `session(action="wait")`; the play directory's allow-list is `mcp__ck3` only
  (and `ck3agent run --allowedTools` the same). Re-running setup on a 2026.10
  play directory removes the old entry and keeps your own rules, with or
  without `--no-allow`.
* **Replies name only tools you can call.** A refusal for an unbound ruler now
  says `session(action="control", character_id=<id>)` (it named a `claim`
  action that does not exist), and target-list errors name
  `discover(what="targets", action=...)`.
* **`session(action="wait")` replies carry no `note`.** It restated the play
  protocol on every call; `waited_s`, `cap_s`, the dates, `clock_ran` and `paused`
  are unchanged. A wait whose game does not unpause within 3 seconds waits for
  nothing and is refused `game_did_not_unpause`.
* **Every tool call the protocol and docs teach is checked against the server.**
  A test reads the registered tools (FastMCP `list_tools`) and fails if
  `CLAUDE.md`, `run-prompt.md`, `CLAUDE-desktop.md`, the README, `docs/*.md`, the
  server's instructions, a tool description or the live-state block names a tool,
  parameter, enum value or `do` argument that does not exist. The `session`
  description's `set_experimental(...)` shorthand is now the real call,
  `session(action="set_experimental", flag=<name>, enabled=<bool>)`, and the
  experimental flags' replies say `unvalidated` instead of "validation PENDING".

* **`discover(what="evaluate_trigger")` is removed.** It could only test a
  trigger against a character, and the tool had no way to name the trigger,
  so every call evaluated an empty trigger and answered nothing a player could
  act on. Read the fact directly instead: `query(domain="character")` or
  `inspect(subject=...)` for a character's or ruler's state, and
  `discover(what="interaction_validity")`, `what="acceptance"` or `cbs` to ask
  whether an action would be allowed. The bridge action
  `evaluate_named_trigger` is gone, and the release no longer ships
  `share/trigger_subclasses.csv`: `install.sh` removes an old copy, `ck3agent
  setup` no longer reports a trigger catalogue, `ck3agent config` no longer
  lists `trigger_csv`, and `launch` no longer sets
  `CK3_TRIGGER_CATALOGUE_CSV`.
* **Interactions take what their window takes.** `do(intent="interact")` now
  applies `secondary_actor_id` and `secondary_recipient_id` (they reached the bridge
  and were dropped, so a marriage or a guardian appointment went out without the
  people it was about), `title_id` as the interaction's title (`grant_titles`,
  `revoke_title`, `buy_claim`), and the new `send_options`: the window's
  checkboxes by flag name, e.g. `["matrilineal"]` or `["hook"]`. A name the
  interaction does not declare is refused with the list it does declare
  (`inspect(subject="pending")`: `rejection_reason: send_option_unknown:<name>`,
  `detail`). The outcome row in `inspect(subject="pending")` carries the
  secondaries, the title, the send options requested and applied
  (`send_options_applied`), and the executor's `detail` line. To marry two
  people of your own court, name yourself as `target_id` with both secondaries;
  a child is betrothed instead of married.
* **You can see a matrilineal marriage.** `inspect(subject="character")` lists
  each living spouse with `matrilineal` (the game's own matrilinear-marriage
  reading) and the character's `is_female`.
* **Disinherit works, down your own line.** `disinherit_interaction` and
  `grant_titles_interaction` were refused by the server
  (`ai_refuses_self_disadvantaging`, now removed). Both are sent; disinherit is
  sent only at your own child, grandchild or great-grandchild (gate
  `target_not_own_descendant` otherwise, nothing sent). `grant_titles` and
  `designate_heir` go straight to the engine instead of through the AI's own
  target choice, which ignored the character you named.
* **Title laws are the title-law window's.** `do(intent="change_law",
  args={"law": ..., "scope": "title", "title_id": ...})` sends the same command
  the game's title-law window sends: the engine takes it (`verdict: "pending"`;
  read it back in `inspect(subject="title_succession").title_laws`) or refuses
  with its own reason (gate `engine_refused`, the text in `blocked.summary`). The
  old route's `high_stakes_law_uncosted` refusal and `allow_uncosted` parameter
  are gone for title laws. `inspect(subject="title_succession")` lists the title's
  own succession laws (`title_laws`).
* **`succession_plan` lists your levers.** Its `fix` block (which said disinherit
  could not work) is replaced by `levers`: each heir's age and whether they are
  your child, the realm's succession laws, whether you can designate an heir and
  which law allows it (or why not), and the call and the read-back for
  designate heir, disinherit, grant and revoke, title law and marriage. Its
  description names that block; it had promised "the single lever that WORKS"
  and a `wont_work` list the reply does not carry.
* **An interaction the engine refuses to send says why, in the game's words.**
  The refusal that read `target_would_reject` for every failed send now names
  which of the engine's checks said no: `send_conditions_not_met` (the
  interaction's own conditions, e.g. a dynast without the renown disinherit
  asks), `target_would_reject` (the recipient declines) or
  `send_cost_unaffordable`. When the engine's own check refuses a send
  (`interaction_not_valid_for_target`, `send_conditions_not_met`),
  `inspect(subject="pending")` carries the text the game's interaction menu
  would show in the row's `detail` (for example a missing realm law, the
  prestige it costs, or a truce), so you know what to fix. A refused send
  option, secondary or title is no longer retried through the AI's own target
  choice, which sent the interaction without them.
* **Engine text reads as words.** Every refusal or reason the game words (a
  title law's `Missing prestige 948`, a build's tooltip, a faction's press
  check) comes without tooltip markup, its character and concept links removed;
  several reasons are separated by "; ". A rejected row in
  `inspect(subject="pending")` always says why: the game's reason, or "the game
  gives no detail" when the game sent none, and an engine refusal whose text
  cleans to nothing says the same.
* **`succession_plan` no longer calls a splitting realm intact.** When the
  succession simulation names no heir, the heirs and the verdict come from the
  per-title map (`heirs_source: "per_title_map"`); at the 867 start East Francia
  read `passes_intact: true` while its map split Bavaria off to a second son.
  `query_character` also carries `betrothed: {id, matrilineal}` and the ruler's
  `designated_heir_id` (where the game keeps it; the title read's
  `designated_heir_id` stayed -1 after a designation took).
* **`succession_plan` says whether designating an heir would go through.**
  `levers.designate_heir` is available only when a realm law allows it, you have
  the 1000 prestige it costs, and some child of yours can be named; it lists those
  children (`candidates`), the ones the engine would refuse and why (`barred`:
  another house, already designated, disinherited, bastard, devoted ...), and the
  cost. An administrative realm pays 500 prestige and 150 influence; influence is
  not read, so there `available` is `null` and `unread` says what the engine
  checks. `levers.grant_title` names the realm law that lets you revoke titles
  (`revoke_allowed_by`) or what your realm lacks, and the engine's limits on revoking
  (not a title granted in the last year; the vassal answers in 4-9 days and may
  rebel instead), and which vassals accept at once (`revoke_accepted_at_once_by`: a
  baron, your prisoner, a vassal you spend a strong hook on, an AI county governor
  of an administrative realm).
* **Dynasty reads name your dynasty.** Several reads took the betrothed's
  character id for the dynasty id (the same field): `query_character`'s
  `dynasty_name`, the observation's dynasty name and `family.dynasty_id`, and the
  dynasty used by kin and side checks. All of them read the dynasty from the
  house now. `inspect(subject="dynasty", character_id=X)` with no `dynasty_id`
  answers X's dynasty (it looked up dynasty 0).
* **Every scheme the game has, with the game's reasons.**
  `discover(what="scheme_types")` lists the schemes in the game's database
  (bridge action `query_scheme_types`, new: what each targets,
  hostile/personal/political/contract, secret, maximum
  breaches). `discover(what="scheme_targets", scheme=..., target_id=...)` asks
  the game whether your ruler can start that scheme on that character, or, with
  `candidates=[...]` or nothing (his court and vassals, his liege and the liege's
  court, the holders of titles he claims and their courts), lists whom he can;
  every refusal is a list of reasons, each `{check, summary}`: the game's own
  words for an existing scheme or a cooldown on that target, and "the game
  gives no detail" for a failing `allow` or `valid` check (the game exposes no
  words for a trigger; no script file or line is reported). A
  `do(intent="scheme")` start runs the same check first (before, a scheme whose
  `allow` or `valid` trigger failed could still be created) and its refusal
  carries the reasons in `blocked.measured.engine_reasons`.
* **Scheme reads tell the truth about success.** `success_chance` in
  `inspect(subject="schemes")`, `inspect(subject="incoming_schemes")` and the
  observation feed was the scheme's secrecy. It is now the game's success
  chance, with `success_chance_max`, and `secrecy`, `breaches`,
  `max_breaches`, `agent_charges`, `exposed` (and `exposed_date`), the scheme's
  `category`, and `agents`: each slot with its agent's id, name and
  contribution. `scheme_type_label`, `danger`, `phase`, `eta_days`,
  `discovery_timer`, `exposure_timer` and `is_discovered` are removed; an
  incoming scheme is `dangerous` when the game classes it hostile. The
  observation feed's scheme rows give `success_chance` the same way inspect
  does (the game's chance in percent, `null` when it cannot be read) beside
  `secrecy`, and their slot count `agents` is renamed `agent_slots`, the name
  inspect uses (inspect's `agents` is the list of slots).
* **A scheme list accounts for every scheme.** `inspect(subject="schemes")` and
  `inspect(subject="incoming_schemes")` carry `invalidated` (schemes the game
  has called off) and `unreadable` (ones the bridge could not read) beside the
  schemes listed, so `total_schemes` is the listed count plus those two.
* **A call from a dead ruler's seat is refused as dead.** It was refused
  `actor_dead_or_stale`, "retry now", with a re-pin that could not succeed; it
  is now `character_dead` (retry never), and a call about a war names the war's
  leaders as they read now, so you can act from a living seat. A call for a
  character that does not exist is `character_not_found`.
* **A read the game's memory could not answer says so.** Where the bridge
  could not read a value it used to answer 0 for it (gold, personality axes, a
  war count, the player's id). A call whose read fails before it has changed
  anything changes nothing: it is refused with `raw_reason` `memory_access_fault`,
  or `character_not_found` when the record that could not be read was the
  character's own. A call that has already made its change (setting personality
  weights, switching the AI on or off) is never refused afterwards: when a read
  after the change fails it answers success with `degraded: true`,
  `fault_count` and the values it wrote, and `verified: false` when the failed
  read was the one that would have confirmed them. A reply that stands without
  the value (a list, a report) carries `degraded: true` with `fault_count`.
* **`do(intent="scheme")` needs the scheme named.** A start without
  `args.scheme` used to start a murder; it is now refused
  `missing_required_argument`. `args.scheme_type` is no longer read (use
  `scheme`). A start or cancel is `applied` only when
  `inspect(subject="schemes")` shows the change. The game gives a cancelled
  scheme's id to the next scheme it starts, so a cancel first reads your
  ruler's schemes (a `scheme_id` he does not hold is refused `name_not_found`
  and nothing is sent), and the read after it looks for that scheme by its
  type, target and start date: when the AI has already started a new scheme
  under the same id, the cancel is `applied` and the reply names the new one
  under `result.id_reused_by`, so you do not cancel it by sending the cancel
  again. A scheme against your own
  ruler is no longer refused by the bridge; the game decides (Expand Power Base
  targets its owner).
* **Reads that changed between two launches of one save now read the same.**
  A war's target titles (`inspect(subject="wars")`) listed the first title
  right and every later one as stray bytes; the active laws
  (`inspect(subject="laws")`) and the succession rule's realm-law fallback
  read one slot past the last law; law groups listed members past their end;
  a title's holder history listed undated rows; a culture's innovation and
  fascination lists took rows from past the array. Traits
  (`query(domain="traits")`) report `minimum_age`/`maximum_age` only where the
  game sets one, with the game's values (rowdy 3..15, sickly at most 6), and
  `ruler_designer_cost` from the right field. (`dominance` was later removed
  from every read and write: the game keeps no such axis.)
* **Removed fields that never held a value:** `piety_cap` and `prestige_cap`
  (CK3 caps neither), a trait's `valid_sex`, `min_value` and `max_value`, a
  building type's `scripted_values`, and the AI controller's `tick_counters` in
  `observe(depth="full")` (they were read from the wrong object).
* **A ruler's vassals are its vassals.** `query_realm`'s `vassals` (and
  `vassal_count`, `inspect(subject="vassals")`, a character's `vassal_count`,
  `direct_vassal_count` in `observe(depth="full")`) listed the ruler's spouse and
  unlanded characters instead of the rulers who hold land under it; they now
  list the subjects of the ruler's vassal contracts, each of whom holds a title
  and names the ruler as liege. The realm walks behind
  `discover(what="valid_cbs")`'s border check and
  `query(domain="conquest_targets")` follow the same vassals.
  `vassal_count_raw` is gone.
* **`set_phase` says whether your ruler's AI is really on.** The bridge reads
  the AI switch back after setting it: `ai_gate.ai_enabled_read_back`, and
  `reasserted` is true only when it reads on. When it could not be re-enabled,
  the warning names `session(action="whoami")`, whose `agency.ai_tick.enabled`
  is the field to read for the bound ruler.
* **Building is a player's building.** `do(intent="build")` used to place the
  building at once and for free. It now gives the game the same order a player
  gives: the game's construction rules decide, and a refusal says what the
  build button's tooltip says ("Missing gold 318; not met: The Swabian Culture
  has the Battlements Innovation"); the price comes out of the treasury, and
  the building takes its construction days. `inspect(subject="county")` shows a holding's
  `construction` (the building, the slot, when it was ordered, how many days,
  `days_left` and `progress_per_day`, the construction days the game takes off it
  each day; `time_factor` is the game's own factor, 1 / that rate).
* **`do(action_class="buildings")` lists buildings.** It had listed nothing.
  Pass `args={"location_id": <barony title id>}` (a holding you hold, from
  `inspect(subject="county")`) and every row is the game's verdict for you
  there, with its price and a `do_call` ready to send.
  `discover(what="construction_check")` gives the same verdict for one
  building. `query(domain="buildings")` reports each building's
  `construction_days`, and `category` is now the building's kind (0 a slot
  building, 1 the duchy capital's, 2 special, 3 great); the field it used to
  show was the construction days, and the `category` filter is gone. The game
  answers the whole list in one call, on the game's own thread, with the game
  paused or running, and so does `do(action_class="fabricate_targets")`. Both run
  the game's checks under a stand-in for its random numbers, because a check
  that drew them could switch off every action that needs the game's random
  numbers for the rest of the session; a list stops asking the game at the
  first row that would, and the rest of its rows say `rng_scope_latched`.
  Both are reads: `inspect(subject="pending")` no longer lists them as finished
  writes.
* **The men-at-arms read counts what the game holds.** The bridge's
  `query_maa_types` (behind `do(action_class="men_at_arms")`) lists the game's
  live men-at-arms types (it read the list's capacity), and for a ruler gives
  `regiment_refs` (`personal`, `title`: the lengths of the realm's two regiment
  lists) and `regiments_unresolved` (regiments the read could not name).
* **Lowering a vassal's obligations works.** `do(intent="set_obligation",
  args={"vassal_id": ..., "obligation": "feudal_government_taxes", "level": 1})`
  found no contract for any vassal and refused every call; it now lowers the
  level on the liege's contract with that vassal, and the game honours it: the
  vassal's opinion of you rises by the level's `subject_opinion` (taxes: normal
  0 to low +5). A vassal you hold no contract with is refused
  `no_such_contract`; an obligation that contract does not have (the reply lists
  the ones it has) and a raise (the game asks the vassal's consent and it costs
  opinion, which this does not do) are refused `target_ineligible`.
  `inspect(subject="vassals")` shows each vassal's `contract`: every obligation
  and its level.
* **Choosing a lifestyle focus and naming a building look only at the game's
  real entries.** `do(intent="lifestyle")` and `build` looked a name up across
  the game list's whole allocated capacity (32 slots for the 28 foci, childhood
  education foci included), reading the empty slots past its last entry; they
  now stop at the last one.
* **Interactions sent right after loading a save no longer fault.** The first
  direct interaction (a gift, for example) could crash the dispatch before the
  game had built its interaction context; the bridge read the wrong
  guard byte and now reads the one the game checks.
* **The Windows VM runbook's live step grades the Windows game against Linux.**
  Step 4 of the Windows VM runbook (since withdrawn) attaches the live suite to
  the Windows game (no lock, no launch, no kill) and grades its replies against
  two Linux runs of the same build on the same DLC-stripped save, which the step
  names; the ADR's phase W4 names the same suite.
* **13 tools.** The `intent` tool is removed: a queued write is now `do`'s
  `verdict: "pending"` with a `pending.intent_id`, `inspect(subject="pending")`
  lists what the bridge still holds and the outcome of what drained, and
  `do(intent="cancel_pending", args={"request_id": <intent_id>})` withdraws
  one (`clear` has no replacement). The bridge keeps the verdicts of the last
  128 drained writes (a 129th drops the oldest, whose token is then found only
  by a read-back); `inspect(subject="pending")` lists the newest `max_results`
  of them (default 20, at most 64). `observe_v2` is renamed `observe`; the old
  `observe` and its `include` parameter are removed, and `observe(depth="full")`
  carries the AI evaluations it read (the personality axes are in `posture`
  at every depth). The code of the tools retired before 2026.10.0
  (`act`, `game`, `control`, `bridge`, `execute_console_command`) is deleted
  too.
* **One reply shape.** `query`, `discover`, `steer`, `pass_turn` and the board
  tools now return the envelope the other tools use:
  `{ok, tool, result, blocked, warnings, errors, bridge_health, game}`. `data`
  is now `result`, `error: {code, message}` is now `blocked` (a gate),
  `tick`/`game_date` are in `game`, and `resolved_character_id` is
  `actor.character_id`. A `query(domains=[...])` batch with a failed domain
  lists it in `errors` and reads `ok: false`.
* **A read that raced a game tick is read again.** A reply the bridge flags
  `settled: false` (a tick crossed the read) is read again, up to 2 times; one
  that still races is returned with the flag on it.
* **Every refusal names its gate.** The gate `unknown` is gone. A bridge
  refusal the server does not map is `unmapped_refusal`, the bridge's code in
  `raw_reason`, and the server's own refusals are named:
  `missing_required_argument`, `invalid_argument`, `unknown_verb`,
  `unknown_subject`, `unknown_domain`, `bridge_disconnected`, `read_timeout`,
  `write_unconfirmed`, `speed_refused_at_war`, `war_cap_reached` and the
  `plan_war`/`observe` tripwires (`faction_power_tripwire`,
  `stress_break_risk`, `stress_climbing`, `low_priority_pruned`).
  `verdict: "dispatched_pending"` is now `pending`, the one spelling, and every
  warning and error is `{code, detail}`.
* **Tool parameters list their values.** `do`'s `intent` and `action_class`,
  `inspect`'s `subject`, `session`'s `action` (now including `add`),
  `set_phase`'s `phase`, `discover`'s `what` and `observe`'s `depth` carry an
  enum built from the code that answers them. `do`'s description had listed
  its intents by hand and missed `set_education_focus`. `do`'s `args`
  description carries the per-intent argument table, read from the code.
  `query`, `inspect`, `cbs` and `session` with their key
  argument missing return `missing_required_argument` instead of a schema error.
* **`declare_war` without `character_id`** matched its own wars against
  character 0, so the "already at war" check never fired, and sent
  `actor_id=0`, which the bridge refused (`missing_required_argument`), so no
  war was declared. It now resolves the bound ruler first.
* **`do(intent="muster")` is gone.** It never raised an army: the game ran the
  call and nothing moved (the engine function behind it is the "raise all
  armies" button's availability check, not the raise). At war the AI raises
  its own levies and prosecutes a declared war on its own; read what is in the
  field with `inspect(subject="armies")`. `muster`'s `mode="raiders"` went
  with it, and so did the bridge actions `raise_armies`, `raise_raiders` and
  its alias `raid`.
* **Make peace.** `do(intent="make_peace", args={"war_id", "terms"})` ends a
  war from either leader's seat: `white_peace`, `surrender` or
  `enforce_demands`. The enemy's own acceptance decides white peace and
  enforced demands (`recipient_refuses` when he would say no, and nothing is
  sent); a surrender is always taken. `applied` means the war is over: the
  reply carries the outcome the game recorded and your gold and prestige
  before and after. A peace sent on a war the game had already ended is
  `war_not_found`, with that war's outcome and end date. A defender can now
  surrender or enforce his demands too,
  and terms no longer wait for the AI's own rules (it offers terms only at
  100 war score). `inspect(subject="war_stance")`'s exit lever names these
  calls instead of a raw interaction.
* **Allies.** `do(intent="call_ally", args={"war_id", "ally_id"})` calls an
  ally into a war you lead, and `do(intent="join_war", args={"war_id",
  "leader_id"})` joins an ally's war on his side; `applied` means the joiner is
  in the war's participant list. `do(intent="interact")` passes `war_id` for
  `call_ally_interaction` and `join_war_interaction`. Only a war leader can make
  peace or call allies (`not_war_leader`); a war you are not in is
  `war_not_found`.
* **`cbs` answers.** Every `cbs` call came back empty with a `cb_query_busy`
  warning after about 24 seconds: the bridge ran the single-target CB check on
  the game thread, which then could not service it. It now runs where the
  batched check behind `plan_war` does.
* **`cbs` lists every kind of war.** It showed only conquest, de jure,
  subjugation, holy war and claim CBs. It now lists every CB the game accepts
  against the target plus every CB of the war families, invasions included
  (`invasion_war`, the Varangian and Viking adventures, the migrations), each
  with its `family` and the game's verdict, and `by_family` groups the
  declarable ones. Event wars the game starts by itself show as dropped
  (`group_script_only`). A filter the bridge copied from the AI's own war
  search (`kind_relation_mode_filter`) no longer hides CBs: it marked
  `invasion_war`, `claim_cb` and `county_conquest_cb` dropped against every
  target you were not allied with or in a truce with, in `cbs`,
  `discover(what="valid_cbs_strict")`, `plan_war` and the conquest targets
  alike; the game's own CB check now decides.
* **`gates_deferred` is now `gates_unchecked`.** In
  `discover(what="valid_cbs_strict")` the list of the game's CB conditions the
  bridge does not check is renamed `gates_unchecked` (`valid_to_start`,
  `ai_only_against_neighbors`, `max_ai_diplo_distance_to_title`,
  `per_cb_allowed_against_character`): a CB that reads valid there can still
  fail one of them. Read the new name; the old one is gone.
* **Conquest targets anywhere.** `plan_war(toward="k_france")` and
  `query(domain="conquest_targets", title_id=...)` list every de-jure duchy
  under the title you name, at any tier and anywhere on the map, first. The
  built-in preference for the British kingdoms is gone (`de_jure_british` and
  `british_count` are replaced by `in_toward` and `toward_count`), and each
  checked holder lists `engine_valid_cbs`, every CB the game accepts against
  him, so a county too far for a conquest war still shows the war that reaches
  it. `plan_war(goal_scoped=false)` now adds the duchies bordering your realm.
  The game's own check now runs whether the game is paused or not: asked
  unpaused, each check waited out its timeout (stalling the game meanwhile)
  and every county's `conquerable_now` was the bridge's guess.
* **War score reads.** Every war's `war_score` read 0 (in `inspect`,
  `query(domain="wars")`, `observe`, `inspect(subject="war_score")` and the
  `war_stance` stall verdict): the bridge summed the game's war-score
  components in the wrong unit and added the defender's occupation and
  imprisonment instead of subtracting them. It now reads the game's sum, and
  the breakdown (imprisonment, battle, occupation, ticking, landless) is in
  war-score points, attacker minus defender.
* **War 0 shows up.** The game numbers its wars from 0, and every
  per-character war read (`inspect(subject="wars")`, `query(domain="wars")`,
  the at-war checks before a declaration) skipped war 0, so its two
  belligerents read as at peace.
* **Every war id is the war's full id.** The faction reads,
  `inspect(subject="war_stance")`, `query(domain="call_to_arms")` and the
  war-create reply behind `declare_war` gave the id without its generation
  byte, which named no war, or a different one once the game reused the slot,
  when passed to the war intents. Every war id a reply gives is now the same
  full id, the one `query(domain="wars")` gives.
* **War names read as words, and `observe` says your side.** Every war name the
  bridge reads has the game's text markup removed. `observe`'s war rows fill
  `my_side` (`attacker` or `defender`); it was always empty.
* **`do(intent="force_offensive_stance")` is gone,** with the
  `force_offensive_stance` experimental flag and the bridge actions
  `force_offensive_stance` and `set_force_offensive_stance`. A war you declare
  is fought by the game's own AI: a declared county conquest had the
  attacker's army in the field within days and a siege of the defender's
  holding within two months, and the stance the intent forced was the one the
  game already held. Read the war's `sieges` with `inspect(subject="wars")`
  and its stance with `inspect(subject="war_stance")`. `set_phase("expansion")`
  writes the expansion personality (read back from the engine); no run
  isolates its effect on how a war is fought: the same war, declared without
  it, was fought at the same pace.
* **A war's sieges are the war's own.** A siege was listed under every war its
  besieger fought, with that war's defender written in. Now a siege is listed
  under the war where the besieger and the besieged holding's realm are on
  opposite sides, with the holding and its holder.
* **`query(domain="de_jure")` and `inspect(subject="title")` need a title.**
  With none named the bridge read Britannia's meter; it now answers
  `title_required`.
* **`plan_war` rows say what the game says.** `win_prob` (always 0.5) is
  replaced by `strength`: your military strength and the target's, with
  allies, and their ratio, read from the game; `score` is de-jure counties
  gained times that ratio. `affordability` is the CB's base cost from the
  bridge, marked `estimate` (the game scales the price by target), and no
  longer holds a row back from `ready_to_declare`. `straddles_succession` and
  `exclude_straddling` now follow `declare_war`'s own rule (the ruler is 62 or
  older); `lifecycle.succession.death_eta_years`, a guess, is removed. The
  `enumerator_omits_conquest_cb` warning, which fired on every call, is gone.
  A claim war you hold no pressed claim for is no longer offered: the game
  refused it (`claim_not_pressed`) after `plan_war` had called it ready.
* **Speed.** `session(action="speed")` refuses a speed below 1
  (`invalid_argument`) and one above 3 at war (`speed_refused_at_war`); it
  sets 1 to 3 as asked, and 3 for a higher speed at peace, as the bridge
  clamps.
* **`inspect(subject="heir_plan")` and `guardian_candidates` name calls that
  exist.** A child or heir whose read names nobody is a `heir_read_failed`
  warning, not a row with id 0. The lever hints name
  `do(intent="set_education_focus", ...)`, `do(intent="lifestyle", ...)` for an
  adult's martial focus (it named the retired `act`), `do(intent="interact",
  ...)` and `inspect(subject="guardian_candidates")` (it named an internal
  function). `inspect(subject="guardian_candidates")` gives an in-court
  candidate the exact call that appoints a guardian (`do(intent="interact")`
  with `educate_child_interaction`, the ward's liege as `target_id`, the
  guardian and the ward as secondaries), marked unvalidated, instead of "once
  the assign lever lands".

* **A name the game has not rendered is marked, not faked.** Only the game
  thread can turn a character's localization key (`E_thelred` for Æthelred)
  into a name, on its next tick; a paused game — the first reads of a
  session — has no tick. The bridge used to answer that miss with the key as
  the `name`. It now omits `name` and sends `name_key` and `name_pending: true`
  (`query_character`, the `characters` listing, marriage candidates,
  claimants, every reply built from the character profile); the MCP server's
  actor echo (`controlled`, `added`, `actor`) carries the same two keys; the
  live-state hook lists the ruler without a name and a note, never the key.
  See the bridge contract, 3.6.

* **Replies that were wrong on Linux**, found by diffing them against the same
  build running under Proton (ADR 0001, phase P5). Succession replies
  (`succession_plan`; the `succession`, `succession_simulation` and partition
  queries) took the title's law-vector pointer for its succession rule, so the
  rule changed from one run to the next; one resolver now reads the title's laws,
  else its holder's realm law. `held_by_target` in war-target replies read a
  field that is zero on nearly every title; it now reads the holder. Known
  innovations were read as instances instead of types, so era counts were bytes
  of a key string. An army's commander was a truncated pointer. Lists with equal
  sort keys now come back in a stable order (mercenaries tie-break by company
  id), and a reply degraded by an unreadable address logs the address instead of
  carrying it. `toggle_ai_subsystem` sent no reply when it succeeded; it
  now answers with the subsystem, the requested state and the flag byte read
  back after the write (P6). Claiming a character (`set_controlled_character`)
  before the game's first tick, e.g. in a game loaded paused, answered
  `memory_access_fault` (the handler called a game function the bridge had not
  resolved yet); those functions are now resolved at load. A pause issued
  shortly after an unpause (including the bridge's own re-pause after a
  deferred query's clock pulse) could be undone on the next tick by the
  unpause's queued second step; a pause now cancels it. The council-task
  observer hook is no longer installed: it hooked the game's task-availability
  check under a setter's signature, so that check's callers got an undefined
  answer; `query_council_tasks` without a character now lists no hook
  assignments (the per-seat read is unchanged). `get_ai_personality` gains
  an `engine` object: the nine axis bytes read from the game, whatever a held
  lock reports in `personality` (additive; no existing key moved).
* **Windows port status** (docs, and code that runs on Windows only): ADR 0001
  records P5, the bridge loaded inside `ck3.exe` under Proton; the Proton
  runbook gains a section on talking to that bridge; `CK3_BRIDGE_PORT`
  (Windows only) is in the environment table. Under Proton the AI-tick hook and
  steering run inside `ck3.exe` (runbook section 8), including the interaction
  trigger query and acceptance and the direct interaction dispatch; a Windows
  installer (`installer/windows/install.ps1`) and the
  P7 real-Windows runbook; the MCP client finds the Windows bridge's
  `endpoint.json` on its own (`CK3_BRIDGE_ENDPOINT` overrides); the public Windows
  build is the full bridge, from Windows offset headers generated at export
  (`re/offsets/windows/`, each naming its input shas and the one ck3.exe it is valid
  for); the research inputs behind them do not ship.
* **Bridge actions nobody called are deleted.** `enable_all_ai` (use
  `toggle_all_ai` with `enabled: true`); `set_vacant_fill_enabled` and the
  vacant-seat fill it armed, so `appoint_councillor` on a vacant seat now
  answers `vacant_seat_fill_unsupported`; the aliases `pending_events` and
  `notifications`. `query_borders`, which faulted, is gone with the
  `borders` query domain (`query(domain="neighbors")` lists bordering realms).
  `add_construction` (use `construct_building`), `declare_war_debug` (use
  `declare_war`) and `list_watchlist` (use `get_controlled_roster`, whose
  `roster` lists the same characters); the alias `pause` (use `pause_game`, or
  `set_pause` with `paused`); the alias `do_call_war_create_core` (the action
  is `call_war_create_core`, which the MCP server already sends).
  `probe_war_orphans` is deleted (its orphan list was always empty), and
  `tick_diff` loses its `war_orphans` channel, which was always empty too. A
  removed name answers `unknown_action`.
* **`break_alliance`, the scripted-effect path and the debug console are
  deleted.** `break_alliance`, `set_break_alliance_enabled`,
  `do(intent="break_alliance")` and the `break_alliance` flag of
  `session(action="set_experimental")`; `execute_scripted_effect`,
  `do(intent="scripted_effect")` and its `console_command_denied` gate;
  `execute_engine_effect`; the debug-build `execute_console_command`;
  `execute_script_block` (it could only start a war: use the `declare_war`
  tool).
* **The bridge `query` action loses its `war_details` and `scheme_details`
  types.** Each answered a fixed note and no data; use `query_wars` and
  `query_schemes`.
* **Three reads published the wrong clock state.** `get_action_polarity`
  answered `tick_write` for `search_characters`,
  `list_pending_interactions` and `query`, so the MCP
  server unpaused for a read the bridge then refused while running. They
  answer `paused` now; `query_conquest_targets` stays `tick_read`.
* **`query_de_jure` is no longer labelled broken.** Checked live: it answers
  by `title_key`, `title_id` or a character's primary title,
  with parents, de-jure children and, given `character_id` or `claimant_id`,
  a held/total count, through `query(domain="de_jure")` too. `inspect` has
  no `de_jure` subject (it answers `unknown subject`); its de-jure read is
  `inspect(subject="title", title_key=...)`.
* **`query_character_memories` drops three fields that carried nothing:**
  `type` (always `null`), `participants_resolved` (always `false`) and
  `memory_type_db_count`. `participant_count` and the other fields are
  unchanged.
* **`enter_observe_mode` says what it does.** Its `notes` called it a
  scaffold and pointed at an internal report; it now says it unpauses
  through the normal unpause path and does not drive the lobby view.
* **A read the bridge declined is a refusal.** `query`, `discover` and
  `steer` showed a bridge reply of `{"success": false, "reason": ...}` as
  `ok: true` with the refusal as the reading. It is now refused with the gate
  its reason names (`character_not_found`, `no_ruler_bound`,
  ...); in a batch or a `steer` read it is an `errors[]` row.
* **`query`**: passing both `domain` and `domains` is refused
  (`invalid_argument`; the description said they exclude each other and the
  call silently dropped `domain`). `event_log` and `relative_power` answer in a
  `domains` batch (they were in its list and came back `unknown_domain`). The
  description now explains every parameter (`since_tick`, `limit`,
  `require_in_range` and the `map_graph` ones were unexplained) and states the
  real `max_results` default for `characters` (50). `query(domain="characters",
  court_of=<ruler>)` is new: only the characters whose immediate liege is that
  ruler (his unlanded courtiers and his landed direct vassals), and the reply
  echoes `court_of`.
* **`discover`'s description explains `building`, `location_id`, `slot`,
  `character_id` and `decisions`.** `discover(what="construction_check")`'s
  `slot` is optional (the bridge reads it from the holding when omitted), so
  slot 0 can be passed. The `found_kingdom` / Danelaw clause that
  said to toggle `DeclareWar` off to reach peace now says to end or white-peace
  your wars.
* **`steer` refuses what it cannot write.** A personality key that is not one
  of the nine axes (the bridge ignored it and reported success) or a value that
  is not an integer from -100 to 100 is refused (`invalid_argument`) and
  nothing is written. A write the bridge refused is an `errors[]` row with a
  named gate.
* **The subsystem toggles are gone.** `steer(subsystems=...)`, the
  `subsystems` section of `observe(depth="full")`, the `DeclareWar`/`Decisions`
  toggles `set_phase` wrote (and its `subsystems`, `declare_war_recorded` and
  `declare_war_note` result fields), and the bridge's
  `set_character_subsystems` / `get_character_subsystems` actions and the
  per-ruler mask they kept are deleted. The game's AI never read the mask
  (every reply said `engine_enforced: false`): turning `DeclareWar` off did
  not stop the AI declaring war and `CallAllies=false` did not stop an ally's
  call to arms. Steer with the personality axes or `set_phase`, and start a
  war yourself with `declare_war`. `get_bridge_state` no longer carries
  `subsystem_masks`.
* **`set_phase` refuses bad `overrides` the way `steer` refuses a bad axis.** A
  key that is not one of the nine axes, or a value that is not an integer from
  -100 to 100, is `invalid_argument` and nothing is written. An unknown key
  (`CallAllies` among them) used to vanish and an out-of-range value
  (`boldness: 500`) reached the bridge. Both tools call one check.
* **`inspect` loses its `include` parameter.** It was accepted and never
  read.
* **`query` reads only what it can be given.** The per-domain builders
  forwarded keys `query` has no parameter for, so no call could set them:
  `scheme_type` (`scheme_inits`),
  `include_expansion`, `max_engine_checks` and `engine_timeout_ms`
  (`conquest_targets`), `claimant_id` (`de_jure`), `title_key` (`dejure`,
  `goal`), `since_date` and `order` (`title_history`). They are deleted; the
  bridge defaults they never overrode still apply.
* **`observe` refuses a `depth` other than `brief` or `full`** (it read any
  other value as `brief`). When the seat holds no title to measure, the
  `de_jure` meters say so (`unavailable`) instead of showing 0 of 0 held, and
  `mint_requirements` says there is no title to measure.
* **`inspect(subject="pending")` lists writes only.** A `discover(what=
  "scheme_targets")` call or an interaction check appeared there as a finished
  write (`befriend: success` for a scheme never started) and took a slot from a
  real write's `intent_id`. A row's `actor_id` is the ruler the write named; a
  `steer` row said 0.
* **Events are not answered by any tool.** `do(intent="respond_event")`,
  `session(action="auto_events")` and `query(domain="event_probabilities")` are
  deleted: no read yielded an event id, and the auto-respond setting acts only
  while no client is connected. The bridge actions behind them are deleted
  too (`respond_event`, `query_event_probabilities` and its alias
  `event_probabilities`, `set_auto_respond_events`, and `query_pending_events`
  with the `query` action's `pending_events` / `notifications` types): nothing
  called them; `get_tick_info`'s `event_pause_count` says whether the game's
  event pause holds the clock. With
  no client connected the bridge still releases the game's own event pause,
  now always (its switch is gone, and `get_tick_info` no longer carries
  `auto_respond_events`). `query(domain="event_log")` rows now carry
  `owner_name` and `target_name`.
* **One door per read.** `do(action_class=...)` owns the enumerations:
  `discover(what="hireable_mercenaries" | "fabricate_targets" |
  "joinable_factions")` are deleted and refuse with `next` naming
  `do(action_class="mercenaries" | "fabricate_targets" | "factions_to_join")`;
  `discover`'s `task` parameter went with them. `query` loses the domains that
  repeat another read: `active_wars` (`query(domain="wars")`,
  `inspect(subject="wars")`), `dejure` and `goal` (`inspect(subject="title")`;
  `de_jure` stays), `partition_preview`, `succession_preview`,
  `succession_simulation` and `debug_flags`; `inspect(subject=
  "succession_preview")` is deleted too. A council task's target is
  `target_title_id` everywhere: the `do_call` rows printed `target_id`, which is
  no longer read.
* **One succession verdict.** `succession_plan`, `observe`'s `succession`
  block and the live-state hook state the same `passes_intact` and `breaker`,
  from one function. `scatters`, `passes_single_heir`, `any_splits`,
  `scatter_breaker` and the `succession_scatters` warning are gone (the warning
  is `succession_not_intact`); `succession_plan.per_title_map` carries each
  title's heir (`titles`) and no verdict of its own. The simulation's heirs are
  now read under the key the bridge sends (`heirs`): they were read as
  `eligible_heirs`, which it never sent, so the verdict fell back to the
  per-title map or to none, and `inspect(subject="heir_plan")` listed no heir
  line.
* **A scheme refusal is words, not a script file.** Each reason of
  `discover(what="scheme_targets")` and of a refused `do(intent="scheme")`
  (`measured.engine_reasons`) is `{check, summary, debug}`: `summary` is the
  game's own text when it gives one (an existing scheme, a cooldown) and "the
  game gives no detail" otherwise (a failing `allow`/`valid` trigger: the game's
  description of it is not read yet); the script file and line are under
  `debug`.

## 2026.10.0 — 2026-10-19

First public release.

* **Bridge**, `libck3_bridge.so` for Linux x86_64, for Crusader Kings III
  1.18.3.1 (Crane), BuildID `e61ff264…`. Loaded with `LD_PRELOAD`. It hooks the
  AI tick, reads game state, writes personality weights and subsystem toggles,
  and serves NDJSON over a Unix socket in a private per-user directory. Release
  builds refuse any other game build at load and leave out the debug verbs.
* **MCP server** with 15 tools over stdio and streamable HTTP. Reads: `observe`,
  `observe_v2`, `inspect`, `query`, `discover`, `cbs`, `plan_war`,
  `succession_plan`. Writes: `set_phase`, `steer`, `declare_war`, `do`,
  `intent`, `session`, `pass_turn`.
* **`ck3agent` CLI**: `setup` finds the game, checks its build and writes a
  play directory with `.mcp.json`, the play protocol, the live-state hook and
  an allow-list. Also `doctor`, `launch`, `serve`, `config`, `version`, and
  `run`, an unattended Claude Code session that records a transcript and a manifest.
* **Several rulers in one session.** `session(action="add")` binds one more
  ruler and keeps the first as the primary (`control` switches); a
  comma-separated `CK3_AGENT_CHARACTER_ID` pre-binds several. The live-state
  hook lists every ruler held, and the `ck3agent run` manifest records them
  at start and stop. One posture per ruler, one game clock. The pre-bind
  waits for the game to be active (`CK3_AGENT_PREBIND_WAIT_S`, default 120 s)
  instead of being refused when the run starts before the save has loaded;
  the manifest notes the tick and date each bind landed.
* **Live-state hook.** A ruler whose name the bridge has not localized yet
  (it answers with the game's key, `E_thelred` for Æthelred, until a tick
  resolves it) is listed without a name and a note, never with the key.
* **Time passes through the server.** `session(action="wait", seconds=N)`
  unpauses, blocks up to 110 s, pauses and returns the dates; the play
  protocol never uses a shell `sleep`, which Claude Code refuses at 30 s or
  more. `ck3agent launch` pauses the game once it is active (`--no-pause`
  keeps it running).
* **Writes.** `do(intent="scheme")` starts and cancels schemes for an AI
  ruler, validated live; `faction_action` and `break_alliance` stay off by
  default behind an operator-armed file. `set_phase` reports whether it
  re-enabled the ruler's AI tick.
* **Reads.** `query` characters listing adds `realm_counties` and
  `held_title_count` and one `is_ai` definition; `neighbors` resolves from a
  character; `do` enumerates men-at-arms types, laws and mercenaries with
  labels; council tasks take and report a `seat`; `discover(war_targets)`
  says it lists candidates and `cbs` is authoritative.
* **Linux tarball** with installer, split debug symbols, SHA256SUMS and build
  provenance attestation; wheel attached to the release.
* **Docs**: getting started, Claude Code, Claude Desktop, scripted runs,
  troubleshooting, generated tool and bridge-action references, the bridge
  contract, architecture, supported versions, research notes, and the
  Windows and macOS port design record.

Known limitations: Linux only; one supported game build; Flatpak and Snap
Steam are unsupported; Claude Desktop connects through `ck3agent serve
--transport http` and `mcp-remote`, except on its Linux beta. Claude never
plays the human's character, not even as heir. The off-goal veto that
`set_phase("expansion")` arms is process-wide. See the research notes.

### Contributors

* Harrison Stebbins
