"""
Prompt construction for the CK3 LLM agent.

- build_system_prompt: assembles system prompt from SOUL + memory + traits
- format_observation: renders bridge observation dict into readable context
- format_event: renders an incoming game event for LLM decision
- format_action_result: one-line summary of a bridge action result
"""


# ---------------------------------------------------------------------------
# System prompt
# ---------------------------------------------------------------------------

# [release export] `_SYSTEM_CORE, build_system_prompt` removed: not part of the MCP server surface.




# ---------------------------------------------------------------------------
# Observation formatting
# ---------------------------------------------------------------------------

def format_observation(obs: dict) -> str:
    """Format a bridge observation dict into readable context for the LLM.

    Handles the rich observation format from the bridge: ruler stats, personality,
    wars, council, schemes, nearby rulers, interactions, decisions.
    """
    parts = []

    tick_id = obs.get("tick_id", "?")
    game_date = obs.get("game_date", "unknown")
    parts.append(f"## Game State — {game_date} (tick {tick_id})")

    ruler = obs.get("ruler")
    if ruler:
        parts.append(_format_ruler(ruler))

        wars = ruler.get("wars", [])
        if wars:
            parts.append(_format_wars(wars))

        council = ruler.get("council", [])
        if council:
            parts.append(_format_council(council))

        schemes = ruler.get("schemes", [])
        if schemes:
            parts.append(_format_schemes(schemes))

    nearby = obs.get("nearby_rulers", [])
    if nearby:
        parts.append(_format_nearby_rulers(nearby))

    interactions = obs.get("available_interactions", [])
    if interactions:
        parts.append(_format_interactions(interactions))

    decisions = obs.get("available_decisions", [])
    if decisions:
        parts.append(_format_decisions(decisions))

    # Minimal fallback (early prototype — just character list)
    ai_chars = obs.get("ai_characters", [])
    if ai_chars and not ruler:
        parts.append(_format_ai_characters(ai_chars, obs.get("ai_character_count", len(ai_chars))))

    return "\n".join(parts)


def _format_ruler(ruler: dict) -> str:
    """Format ruler stats block."""
    lines = ["\n### Your Ruler"]

    name = ruler.get("name", f"Character {ruler.get('id', '?')}")
    house = ruler.get("house_name", "")
    dynasty = ruler.get("dynasty_name", "")
    display_name = name
    if house:
        display_name += f" of {house}"
    elif dynasty:
        display_name += f" of {dynasty}"
    lines.append(f"**{display_name}** (ID: {ruler.get('id', '?')})")

    # Core stats
    stats = []
    for key, label in [
        ("gold", "Gold"), ("prestige", "Prestige"), ("piety", "Piety"),
    ]:
        val = ruler.get(key)
        if val is not None:
            stats.append(f"{label}: {val:.0f}" if isinstance(val, float) else f"{label}: {val}")
    if stats:
        lines.append("- " + " | ".join(stats))

    # Domain & realm
    domain = ruler.get("domain_size", "?")
    domain_limit = ruler.get("domain_limit", "?")
    realm = ruler.get("realm_size", "?")
    military = ruler.get("military_strength",
                         ruler.get("military_current", "?"))
    mil_max = ruler.get("military_max")
    mil_str = f"{military}/{mil_max}" if mil_max is not None else str(military)
    lines.append(f"- Domain: {domain}/{domain_limit} | Realm size: {realm} | Military: {mil_str}")

    # Health & stress
    health_parts = []
    for key, label in [
        ("age", "Age"), ("health", "Health"), ("stress", "Stress"), ("dread", "Dread"),
    ]:
        val = ruler.get(key)
        if val is not None:
            health_parts.append(f"{label}: {val}")
    if health_parts:
        lines.append("- " + " | ".join(health_parts))

    # War status
    if ruler.get("is_at_war"):
        lines.append("- **AT WAR**")

    # Traits — bridge emits trait_names (flat string list) and traits (detailed objects)
    traits = ruler.get("trait_names", [])
    if not traits:
        raw_traits = ruler.get("traits", [])
        traits = [t["name"] if isinstance(t, dict) and "name" in t else str(t)
                  for t in raw_traits]
    if traits:
        lines.append(f"- Traits: {', '.join(traits)}")

    # AI personality scores
    personality = ruler.get("personality", {})
    if personality:
        p_str = ", ".join(f"{k}={v}" for k, v in personality.items())
        lines.append(f"- Personality: {p_str}")

    # Title
    if ruler.get("primary_title"):
        lines.append(f"- Primary title: {ruler['primary_title']}")

    # Family — bridge nests spouse/children/heirs inside "family" sub-object
    family = ruler.get("family", {})
    family_parts = []
    spouse_id = ruler.get("spouse_id") or family.get("spouse_id")
    if spouse_id and spouse_id not in (0, None):
        family_parts.append(f"Spouse: {spouse_id}")
    heir_id = ruler.get("heir_id")
    if not heir_id:
        heirs = family.get("heirs", ruler.get("heirs", []))
        if heirs:
            heir_id = heirs[0]
    if heir_id and heir_id not in (0, None):
        family_parts.append(f"Heir: {heir_id}")
    if ruler.get("vassal_count") is not None:
        family_parts.append(f"Vassals: {ruler['vassal_count']}")
    children = family.get("children", [])
    if children:
        family_parts.append(f"Children: {len(children)}")
    if family_parts:
        lines.append("- " + " | ".join(family_parts))

    return "\n".join(lines)


def _format_wars(wars: list[dict]) -> str:
    lines = ["\n### Active Wars"]
    for w in wars:
        # Bridge format: war_id, attacker_id, defender_id, war_score, battle_score, battle_count
        # Legacy/mock format: attacker (bool), target_name/target_id, war_score, cb_type
        if "attacker" in w:
            # Legacy/mock format
            role = "Attacker" if w.get("attacker") else "Defender"
            target = w.get("target_name", w.get("target_id", "?"))
        else:
            # Bridge format — show both sides
            role = ""
            att = w.get("attacker_id", "?")
            defe = w.get("defender_id", "?")
            target = f"attacker={att} vs defender={defe}"
        war_score = w.get("war_score", "?")
        cb = w.get("cb_type", "")
        battle_score = w.get("battle_score")
        battle_count = w.get("battle_count")

        parts = [f"- War"]
        if w.get("war_id"):
            parts[0] += f" #{w['war_id']}"
        if role:
            parts.append(f"({role}) vs **{target}**")
        else:
            parts.append(f"[{target}]")
        parts.append(f"— score: {war_score}")
        if cb:
            parts.append(f", CB: {cb}")
        if battle_count:
            parts.append(f", battles: {battle_count} (battle score: {battle_score})")
        lines.append(" ".join(parts))
    return "\n".join(lines)


def _format_council(council: list[dict]) -> str:
    lines = ["\n### Council"]
    if not council:
        lines.append("*(council data not yet available from bridge)*")
        return "\n".join(lines)
    for c in council:
        position = str(c.get("position", "?")).replace("councillor_", "").title()
        name = c.get("name", f"char {c.get('character_id', '?')}")
        skill = c.get("skill", "?")
        lines.append(f"- {position}: {name} (skill: {skill})")
    return "\n".join(lines)


def _format_schemes(schemes: list[dict]) -> str:
    lines = ["\n### Active Schemes"]
    if not schemes:
        lines.append("*(no active schemes)*")
        return "\n".join(lines)
    for s in schemes:
        target = s.get("target_name", s.get("target_id", "?"))
        scheme_type = s.get("type", s.get("scheme_type", "?"))
        progress = s.get("progress", "?")
        phase = s.get("phase")
        owner = s.get("owner_id", s.get("owner", ""))
        detail_parts = [f"progress: {progress}%"]
        if phase:
            detail_parts.append(f"phase: {phase}")
        if owner:
            detail_parts.append(f"owner: {owner}")
        lines.append(
            f"- {scheme_type} vs **{target}** "
            f"({', '.join(detail_parts)})"
        )
    return "\n".join(lines)


def _format_nearby_rulers(nearby: list[dict]) -> str:
    lines = ["\n### Nearby Rulers"]
    for r in nearby[:12]:
        # Bridge emits name, house_name, dynasty_name as separate fields
        name = r.get("name", f"ID {r.get('id', '?')}")
        house = r.get("house_name", "")
        if house:
            name = f"{name} of {house}"
        flags = []
        if r.get("is_at_war"):
            flags.append("AT WAR")
        if r.get("alliance"):
            flags.append("ALLY")
        flag_str = f" [{', '.join(flags)}]" if flags else ""
        lines.append(
            f"- **{name}**: "
            f"opinion={r.get('opinion', '?')}, "
            f"military={r.get('military_strength', '?')}, "
            f"realm={r.get('realm_size', '?')}, "
            f"relation={r.get('relation', '?')}"
            f"{flag_str}"
        )
    if len(nearby) > 12:
        lines.append(f"  ... and {len(nearby) - 12} more")
    return "\n".join(lines)


def _format_interactions(interactions: list[dict]) -> str:
    lines = ["\n### Available Interactions"]
    for i in interactions[:15]:
        # Handle both bridge format (hash + name) and enriched format (type + target + accept)
        name = i.get("type") or i.get("name", "?")
        # Clean up interaction names for readability
        display_name = name.replace("_interaction", "").replace("_", " ").title()

        target = i.get("target_name", i.get("target_id", ""))
        accept = i.get("will_accept")
        ai_score = i.get("ai_score")

        parts = [f"- {display_name}"]
        if target:
            parts.append(f"→ **{target}**")
        extras = []
        if ai_score is not None:
            extras.append(f"score: {ai_score}")
        if accept is not None:
            extras.append(f"accept: {'yes' if accept else 'no'}")
        if i.get("hash"):
            extras.append(f"hash: {i['hash']}")
        if extras:
            parts.append(f"({', '.join(extras)})")
        lines.append(" ".join(parts))
    if len(interactions) > 15:
        lines.append(f"  ... and {len(interactions) - 15} more")
    return "\n".join(lines)


def _format_decisions(decisions: list[dict | str]) -> str:
    lines = ["\n### Available Decisions"]
    for d in decisions[:10]:
        if isinstance(d, dict):
            name = d.get("id") or d.get("name", "?")
            desc = d.get("description", "")
            display = name.replace("_decision", "").replace("_", " ").title()
            line = f"- {display}"
            if desc:
                line += f": {desc}"
            lines.append(line)
        else:
            lines.append(f"- {d}")
    return "\n".join(lines)


def _format_ai_characters(ai_chars: list[dict], count: int) -> str:
    lines = [f"\n### AI Characters: {count} total"]
    for c in ai_chars[:5]:
        lines.append(
            f"- ID {c.get('id', '?')}: "
            f"alive={c.get('alive', '?')}, "
            f"has_realm={c.get('has_realm', '?')}"
        )
    if len(ai_chars) > 5:
        lines.append(f"  ... and {len(ai_chars) - 5} more")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Event formatting
# ---------------------------------------------------------------------------

def format_event(event: dict) -> str:
    """Format an incoming game event for LLM decision-making.

    Expected event dict fields:
        event_type: str — event identifier (e.g., "event_popup", "war_declared")
        title: str — event title shown in-game
        description: str — event body text
        options: list[dict] — available choices, each with:
            id: str/int — option identifier (pass as `option` to act(type="event"))
            text: str — option label
            tooltip: str — effect description (optional)
        character_id: int — relevant character (optional)
        requires_response: bool — whether this needs an explicit choice
    """
    parts = []

    event_type = event.get("event_type", "unknown")
    title = event.get("title", event_type.replace("_", " ").title())
    parts.append(f"## Event: {title}")
    parts.append(f"*Type: {event_type}*")

    desc = event.get("description", "")
    if desc:
        parts.append("")
        parts.append(desc)

    options = event.get("options", [])
    if options:
        parts.append("\n### Your Options")
        for i, opt in enumerate(options):
            text = opt.get("text", opt) if isinstance(opt, dict) else str(opt)
            option_id = opt.get("id", i) if isinstance(opt, dict) else i
            tooltip = opt.get("tooltip", "") if isinstance(opt, dict) else ""

            line = f"{i + 1}. **{text}**"
            if tooltip:
                line += f" — *{tooltip}*"
            line += f"  (option_id: {option_id})"
            parts.append(line)

        parts.append(
            "\nChoose an option with `act(type=\"event\", event_id=..., "
            "instance_id=..., option=<index>)`. The game will unpause after your response."
        )
    elif event.get("requires_response"):
        parts.append("\n*This event requires your attention but has no explicit options.*")

    return "\n".join(parts)


# ---------------------------------------------------------------------------
# Action result formatting
# ---------------------------------------------------------------------------

def format_action_result(msg: dict) -> str:
    """One-line summary of a bridge action result for logging."""
    status = "OK" if msg.get("success") else "FAILED"
    action = msg.get("action", "")
    message = msg.get("message", "")
    if action:
        return f"[{status}] {action}: {message}"
    return f"[{status}] {message}"
