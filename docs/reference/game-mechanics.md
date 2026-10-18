# CK3 Game Mechanics Cheat Sheet

Quick reference for key mechanics the agent needs to reason about. This is not comprehensive —
it covers the systems most relevant to AI personality steering.

---

## Power Structure

**Title Tiers:** Barony < County < Duchy < Kingdom < Empire
- You can hold multiple titles at any tier
- Domain limit: how many counties you can personally hold (exceeded = penalties)
- Vassal limit: how many direct vassals you can have (exceeded = penalties)
- De jure: the "intended" territory of a title. Holding de jure land of another's title
  creates tension. Your vassals want their de jure liege, not you.

**Opinion System:** Every character has an opinion of you (-100 to +100). Vassals with low
opinion are more likely to join factions, refuse council positions, and rebel. Opinion is
driven by: traits, relationship, dread, laws, actions taken, religious alignment.

**Dread:** Fear-based control. High dread suppresses faction joining but doesn't create
loyalty. It's a short-term tool. Characters with the brave trait ignore dread.

---

## War & Military

**Casus Belli (CB):** You CANNOT declare war without a valid CB. Sources:
- Claims (pressed/unpressed, personal/vassal) — from fabrication, inheritance, events
- De jure claims — if you hold a duchy/kingdom title, you have CB on de jure territory
- Holy wars — against rulers of different faith (requires piety)
- Conquest CBs — from specific perks or cultural traditions

**War Score:** -100 to +100. Driven by battles, sieges, occupation, ticking war goals.
At +100 you can enforce demands. At -100 enemy enforces theirs. Most wars are won by
occupying the war goal (the specific target county/duchy).

**Armies:** Levies (quantity, weak) + Men-at-Arms (quality, expensive) + Knights (elites).
Military strength shown in tools is total levy + MAA capacity. Actual raised strength
may be lower if levies are depleted or armies are split.

**Alliances:** Formed through marriage. Allied rulers will join your wars (and call you
into theirs). Critical for fighting stronger opponents. Alliance strength = their military
joining yours.

---

## Economy

**Gold Income:** From domain counties, vassal taxes, and occasional events.
- Domain income is your primary source — development level increases it
- Vassal tax depends on your tax law and their opinion of you
- Buildings in your domain increase income (and military)

**Prestige:** Social currency. Spent on: declaring certain wars, some decisions, creating
titles. Earned from: holding titles, winning wars, lifestyle perks.

**Piety:** Religious currency. Spent on: holy wars, religious decisions, requesting claims
from the Pope. Earned from: pilgrimages, holy sites, zealous actions.

---

## Succession

**Partition Types:**
- Confederate Partition: Worst. Titles are split among ALL eligible heirs AND new titles
  may be created. Your realm fractures on death.
- Partition: Titles split among heirs but no new titles created. Still bad.
- High Partition: Primary heir gets at least half. Manageable.
- Primogeniture: One heir gets everything. The goal. Requires late-game innovations.

**Succession Strategy:**
- If stuck with partition: have few children (only 1 son ideal), or... manage the extras
- Disinherit costs prestige but removes an heir cleanly
- Elective succession laws let you influence the vote (useful for keeping realms together)
- Murder of extra heirs is effective but risky (tyranny if caught)

---

## Factions

**Types:** Independence, Liberty (reduce crown authority), Claimant (install a different
ruler), Populist (peasant revolts in counties).

**Discontent:** Factions press demands when their military power exceeds a threshold
relative to yours. Discontent percentage reflects how close they are.

**Prevention:** High opinion, high dread, alliances (allied vassals won't join factions),
hooks (forced vassals won't join), and simply being militarily stronger.

---

## Schemes

**Hostile:** Murder, abduct, fabricate hook. Success chance based on your intrigue vs.
target's spymaster skill. Higher scheme power = better odds.

**Personal:** Seduce, befriend, sway. For relationship manipulation.

Schemes take months to years. The AI manages scheme execution — you influence whether
the AI starts schemes through personality weights (low honor + high energy = more scheming).

---

## Lifestyle & Perks

5 lifestyle trees: Diplomacy, Martial, Stewardship, Intrigue, Learning.
Each has 3 focus tracks with perks that unlock powerful abilities.

**Key perks to know about:**
- Stewardship → Golden Obligations: hooks generate gold
- Diplomacy → Thoughtful: +diplomacy per ongoing alliance
- Martial → Organized March: faster army movement
- Intrigue → Truth is Relative: fabricate hooks faster
- Learning → Whole of Body: health boost (survival)

The AI chooses lifestyle focus. You can influence this through the Lifestyles subsystem
toggle and energy/rationality weights.

---

## Council

5 positions: Chancellor, Marshal, Steward, Spymaster, Court Chaplain.
Each can be assigned a task.

**IMPORTANT — claim fabrication is the COURT CHAPLAIN's task, NOT the Chancellor's**
(verified by RE in this build: `task_fabricate_claim` routes to the Court Chaplain
seat). So to fabricate a claim you must have a **Court Chaplain seated**; an empty
chaplain seat means `task_fabricate_claim` has no one to execute it and NO claim is
ever produced (this silently stalled fabrication in a live run — see L29). Before/while
assigning fabrication: confirm the chaplain seat is filled (`query(domain="council")`
shows `vacant_positions`), and fabrication speed scales with the chaplain's **Learning**,
not the Chancellor's Diplomacy.

Council skill directly affects task speed/success. Place your best people here — and yes,
powerful vassals expect council seats. Denying them tanks their opinion.

The AI manages council assignments. You influence priorities through subsystem toggles
and personality weights.
