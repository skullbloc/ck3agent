# AI Personality Weights Reference

The 10 personality axes are your PRIMARY steering mechanism. Values range from -100 to +100.
Partial updates — only specify axes you want to change. The native AI interprets these
weights across ALL its decisions: war, diplomacy, marriage, council, schemes, everything.

---

## Axis Reference

### honor (-100 to +100)
**+high:** Keeps alliances, honors truces, avoids tyrannical actions, reluctant to scheme
**-low:** Breaks alliances freely, ignores truces, comfortable with tyranny, schemes readily
**Sociopath note:** Keep moderate-positive publicly (maintains Clueless loyalty) but understand
that honor is a *reputation asset*, not a moral principle. Spend it when ROI is clear.

### greed (-100 to +100)
**+high:** Hoards gold, raises taxes, reluctant to spend on gifts/bribes, tight with titles
**-low:** Generous with gold, gifts freely, distributes titles, invests in relationships
**Sociopath note:** Capital allocation. High greed = building war chest. Low greed = investing
in social capital. Toggle based on whether you need liquidity or loyalty right now.

### rationality (-100 to +100)
**+high:** Calculates risk carefully, avoids long-shot schemes, prefers safe options
**-low:** Impulsive, takes risky gambles, emotional decision-making
**Sociopath note:** Keep high almost always. A Sociopath's edge IS rationality. Only lower
when you want the AI to take a specific gamble you've identified as +EV.

### energy (-100 to +100)
**+high:** Actively pursues expansion, starts schemes, seeks marriages, busy
**-low:** Passive, waits for opportunities, rarely initiates, turtle mode
**Sociopath note:** Your "activity level" dial. High during expansion phases, low during
consolidation. Think of it as "are we in growth mode or maintenance mode?"

### boldness (-100 to +100)
**+high:** Declares wars, picks fights with stronger opponents, aggressive schemes
**-low:** Avoids conflict, only fights when clearly superior, defensive posture
**Sociopath note:** The MOST DANGEROUS axis. High boldness = the AI will start wars you
may not want. Use surgically. Raise it ONLY when you have a specific target identified
and the capability to win. Otherwise keep moderate-to-low.

### zeal (-100 to +100)
**+high:** Pursues holy wars, converts heathens, donates to church, religious fervor
**-low:** Ignores religion, tolerant, cynical about faith, won't crusade
**Sociopath note:** Religion is an organizational fiction — perhaps the most powerful one.
High zeal is useful when you need the Pope's support or a holy war casus belli. Otherwise
keep low. Crusades are expensive company offsites with unclear ROI.

### vengefulness (-100 to +100)
**+high:** Targets characters who wronged them, holds grudges, retaliatory
**-low:** Forgives and forgets, doesn't retaliate, moves on
**Sociopath note:** Grudges are an emotional luxury a Sociopath can't afford. Keep low
UNLESS you need to make an example of someone to maintain organizational discipline.
Then spike it temporarily for the specific retaliation, and reset.

### compassion (-100 to +100)
**+high:** Merciful to prisoners, avoids cruel actions, kind to vassals
**-low:** Cruel, executes prisoners, comfortable with harsh measures
**Sociopath note:** Compassion is a STRATEGIC TOOL, not a weakness. High compassion keeps
your Clueless vassals loyal and avoids tyranny penalties. Only drop it when you're making
a calculated terror play (dread-based control). Most of the time, moderate-high is optimal.

### sociability (-100 to +100)
**+high:** Seeks alliances, arranges marriages, attends feasts, diplomatic
**-low:** Isolationist, avoids entanglements, keeps to themselves
**Sociopath note:** Networking. High during expansion and when building the alliance
portfolio. Lower when you need to focus internally. A Sociopath always wants more
connections — more connections = more optionality.

### dominance (-100 to +100)
**+high:** Controls vassals tightly, changes laws, centralizes power
**-low:** Laissez-faire, loose vassal management, decentralized
**Sociopath note:** Organizational control. High dominance = micromanagement, which can
trigger faction pushback. Use when you're strong enough to absorb the discontent.
Lower when your position is fragile. Think "am I strong enough to reorganize right now?"

---

## Common Profiles

### "The Consolidator" (post-expansion stabilization)
```json
{"rationality": 70, "energy": -20, "boldness": -40, "compassion": 50, "sociability": 40, "dominance": 30}
```

### "The Acquirer" (aggressive expansion)
```json
{"energy": 70, "boldness": 60, "greed": -20, "sociability": 50, "rationality": 40}
```

### "The Schemer" (internal power plays)
```json
{"honor": -40, "rationality": 80, "boldness": 20, "energy": 40, "compassion": -30}
```

### "The Diplomat" (alliance building phase)
```json
{"sociability": 80, "honor": 50, "compassion": 40, "energy": 30, "boldness": -30}
```

### "The Crisis Manager" (faction revolt / war defense)
```json
{"rationality": 80, "boldness": 30, "dominance": 60, "energy": 50, "compassion": -20}
```

### "The Successor" (preparing for leadership transition)
```json
{"rationality": 70, "energy": -10, "greed": 40, "dominance": 40, "boldness": -50}
```
