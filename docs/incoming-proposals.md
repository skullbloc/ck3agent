# Incoming proposals

When another ruler's AI makes an offer to a ruler Claude plays (a marriage, an
alliance, a call to war, a peace offer, vassalization), the game can hold the
offer until Claude answers it, instead of the AI answering for Claude's ruler.

**Status: experimental.** The game side is a small mod, `ck3agent_proposals`,
installed by hand ([below](#install-it-by-hand)). Part of it is generated on
your machine from your installed game's own interaction files, so it matches
your game build; those copies never ship. Held and answered in a running game
so far: the AI's own marriage offers and calls to war (an alliance offer was
captured once). Not yet seen in a running game: an AI peace offer held, and an
answer to an offer the game had already voided, read back.

Two switches, both needed. Without the server's declaration
(`CK3_PROPOSAL_MOD=1` in the environment of `ck3agent serve`) nothing changes:
`observe` carries no `incoming_proposals` block, and both calls below are
refused `proposal_mod_not_declared`. With the declaration but without the mod
enabled in the game, the block's `unread` reads `proposal_mod_not_loaded`, the
calls are refused `proposal_mod_not_loaded`, and nothing is held.

## What Claude sees

`observe` carries `incoming_proposals`:

- `offers`: each open offer with its `id`, `class`, who it is `from`, what is
  `offered` (read from your side: for peace, `you_win`, `white_peace` or
  `you_lose`), when it `arrived`, `default_at_lapse` and the exact call to
  answer it (`answer_with`).
- `answered_since_last_turn`: offers answered without Claude: by a rule, at the
  turn boundary, at the hard lapse, or void (the sender died, the war ended).
- `policies`: the standing answer per class, and whether it was set or is the
  default (`ask_me`).
- `unread`: why the offers could not be read. An unread list is never shown as
  "no offers".

## Answering

```
do(intent="answer_proposal", args={"proposal_id": 12, "answer": "accept"})
```

`answer` is `accept`, `decline`, or `native` (hand the offer back to the game's
AI). `native` exists only for marriage, alliance and vassalage; a call to war or
a peace offer is accepted or declined. `applied` means the game was read back
(the marriage or betrothal, the alliance, the war joined, the war ended, the
new liege; for a decline, the offer gone and its resolution logged). An id
Claude was not shown is refused `not_your_proposal` before anything is sent.

Immediately before any answer is carried out, the game checks the offer against
the interaction's own conditions as they stand now (a child betrothed elsewhere
in the meantime, an ally already in the war). An offer that no longer meets them
is void: the answer is refused `interaction_invalid` (raw reason `invalid_now`),
nothing is carried out, and the sender is free to offer again. An answer to an
offer the game has already voided is refused with the void's own reason
(`proposal_closed`: `void_sender_dead`, `void_subject_dead`, `void_war_ended`,
`void_recipient_dead`), and the reply's `void` names it; nothing is sent when
the server already saw the void. Either way the offer is gone: do not answer it
again.

## Standing answers

```
do(intent="set_policy", args={"policy": "proposals", "class": "alliance", "answer": "decline"})
```

Classes: `marriage`, `alliance`, `call_to_war`, `peace`, `vassalage`. Answers:
`accept` and `decline` (the game answers by that rule at once and logs the
rule), `ask_me` (the default: the offer waits for Claude), `native` (the game's
AI answers, as without the mod). Another class is refused `class_not_supported`.

## Silence

An offer still open at the first `observe` of the turn after the one that showed
it is answered with its `default_at_lapse`: `native` for marriage, alliance and
vassalage, `accept` for a call to war, `decline` for peace. An offer is held at
most 400 game days in any case. Outside a scheduled run there are no turns,
and only the 400-day lapse applies.

## Install it by hand

The release tarball carries the mod's fixed files in
`proposal_mod/ck3agent_proposals/` and the generator of the rest in
`proposal_mod/proposal_overrides.py`. `ck3agent` does not install it; these
steps do it by hand.

The game `ck3agent launch` starts uses its own user-data directory,
`${XDG_STATE_HOME:-~/.local/state}/ck3agent/game/`
([files and paths](reference/files-and-paths.md)). As for the
[policy mod](standing-policies.md), the steps give that game a `mod/` folder of
its own (it then no longer sees your other mods); both mods can be enabled
together. `CK3` is the game's install, the directory holding `game/` and
`launcher/`.

With the game closed, from the unpacked release directory:

```bash
G="${XDG_STATE_HOME:-$HOME/.local/state}/ck3agent/game"
CK3="${CK3_GAME_INSTALL:-$HOME/.local/share/Steam/steamapps/common/Crusader Kings III}"
mkdir -p "$G"
[ -L "$G/mod" ] && rm "$G/mod"      # removes the link only; your mods stay where they are
mkdir -p "$G/mod"
rm -rf "$G/mod/ck3agent_proposals"
cp -R proposal_mod/ck3agent_proposals "$G/mod/"
python3 proposal_mod/proposal_overrides.py generate "$CK3" "$G/mod/ck3agent_proposals"
mv "$G/mod/ck3agent_proposals/proposal_manifest.json" "$G/mod/ck3agent_proposals.manifest.json"
{ cat proposal_mod/ck3agent_proposals/descriptor.mod
  echo "path=\"$G/mod/ck3agent_proposals\""; } > "$G/mod/ck3agent_proposals.mod"
python3 - "$G/dlc_load.json" <<'EOF'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text()) if p.is_file() else {}
mods = d.setdefault("enabled_mods", [])
if "mod/ck3agent_proposals.mod" not in mods:
    mods.append("mod/ck3agent_proposals.mod")
d.setdefault("disabled_dlcs", [])
p.write_text(json.dumps(d))
EOF
```

`proposal_overrides.py generate` reads the game's
`game/common/character_interactions/` and writes into the mod whole copies of
the files that define the five kinds of offer, each identical to the game's file
except for added lines ending in `# ck3agent:proposal`, and two generated
`zz_ck3agent_proposal_kinds.txt` files. It prints a manifest (the game version,
each source file's and each written file's sha256), kept beside the mod. It
refuses, naming the reason, rather than write a copy it cannot make exactly (an
interaction missing or defined twice, an unreadable game version). After a game
update, run the steps again.

Then declare the mod to the server. In your play directory's `.mcp.json`
(`~/ck3agent-play` unless you chose another), give the `ck3` server the
environment variable:

```bash
python3 - ~/ck3agent-play/.mcp.json <<'EOF'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text())
d["mcpServers"]["ck3"].setdefault("env", {})["CK3_PROPOSAL_MOD"] = "1"
p.write_text(json.dumps(d, indent=2))
EOF
```

## Check it

Launch as usual. The game's log, `$G/logs/debug.log`, has the line

```
ck3agent_proposals|mod/ck3agent_proposals.mod|Enabled
```

and `observe` carries `incoming_proposals` with `mod_enabled: true` and
`unread: null`. If the line is
missing, check the descriptor's name and `path`, and that `enabled_mods` names
`mod/ck3agent_proposals.mod`.

## Remove it

With no offer open, delete `$G/mod/ck3agent_proposals/`,
`$G/mod/ck3agent_proposals.mod` and `$G/mod/ck3agent_proposals.manifest.json`,
take `mod/ck3agent_proposals.mod` out of `$G/dlc_load.json`, and remove
`CK3_PROPOSAL_MOD` from `.mcp.json`.
