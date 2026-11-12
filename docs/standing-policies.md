# Standing policies and the policy mod

A standing policy is something a vassal keeps doing between Claude's turns,
without a tool call each time:

| Policy | What the game does every 3 days |
|--------|---------------------------------|
| `join_liege_wars` | joins each war the ruler's liege leads, on the liege's side, when the game's own join-war checks allow it |
| `stay_out_of_liege_factions` | leaves a faction against the liege, and keeps the ruler from joining another |

Claude sets one with `do(intent="set_policy", args={"policy": "join_liege_wars",
"on": true})` (`"on": false` clears it). `observe` shows them under
`standing_policies`: each policy on or off, `mod_enabled`, and how to set one.

## The mod

The game holds a policy through a small mod, `ck3agent_policies`: one hidden
event that re-checks every 3 days while the ruler carries a policy. The bridge
sets the policy (a character variable) and fires the event once. The mod
changes nothing for a character without a policy.

Without the mod, `set_policy` is refused `policy_mod_not_loaded`: the game has
not logged the mod enabled, so a policy would not hold. Everything else works
without it.

The release tarball carries it in `policy_mod/ck3agent_policies/`
(`descriptor.mod` and `events/ck3agent_policy_events.txt`). `ck3agent` does not
install it; these steps do it by hand.

## Install it by hand

The game `ck3agent launch` starts uses its own user-data directory,
`${XDG_STATE_HOME:-~/.local/state}/ck3agent/game/`
([files and paths](reference/files-and-paths.md)). Its `mod/` is a link to your
own CK3 `mod/` folder. To keep your own CK3 directory untouched, give the
launched game a `mod/` folder of its own. That game then no longer sees your
other mods.

With the game closed, from the unpacked release directory:

```bash
G="${XDG_STATE_HOME:-$HOME/.local/state}/ck3agent/game"
mkdir -p "$G"
[ -L "$G/mod" ] && rm "$G/mod"      # removes the link only; your mods stay where they are
mkdir -p "$G/mod"
cp -R policy_mod/ck3agent_policies "$G/mod/"
{ cat policy_mod/ck3agent_policies/descriptor.mod
  echo "path=\"$G/mod/ck3agent_policies\""; } > "$G/mod/ck3agent_policies.mod"
python3 - "$G/dlc_load.json" <<'EOF'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
d = json.loads(p.read_text()) if p.is_file() else {}
mods = d.setdefault("enabled_mods", [])
if "mod/ck3agent_policies.mod" not in mods:
    mods.append("mod/ck3agent_policies.mod")
d.setdefault("disabled_dlcs", [])
p.write_text(json.dumps(d))
EOF
```

The descriptor must be `mod/ck3agent_policies.mod` inside that directory, and
`path` must be absolute. `dlc_load.json` there is the launched game's own copy of
your DLC settings; the step adds the mod to its `enabled_mods` and keeps the rest.

## Check it

Launch as usual. The game's log, `$G/logs/debug.log`, has the line

```
ck3agent_policies|mod/ck3agent_policies.mod|Enabled
```

and `observe` reads `standing_policies.mod_enabled: true`. If the line is
missing, check the descriptor's name and `path`, and that `enabled_mods` names
`mod/ck3agent_policies.mod`.

## Remove it

Delete `$G/mod/ck3agent_policies/` and `$G/mod/ck3agent_policies.mod`, and take
`mod/ck3agent_policies.mod` out of `$G/dlc_load.json`. To get your own mods back
in the launched game, delete `$G/mod` (if it holds nothing else of yours); the
next `ck3agent launch` links your `mod/` folder again.
