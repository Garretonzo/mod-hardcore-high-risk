# mod-hardcore-high-risk

Raises the stakes of [mod-challenge-modes](https://github.com/ZhengPeiRu21/mod-challenge-modes) hardcore for players and playerbots.

## Features

- **Loot drop.** When a hardcore character dies, all equipped gear, bags, bag contents, keyring and (optionally) gold drop into chests next to the body. Anyone can loot them.
  - "Gear of the Fallen" holds equipped gear and gold, and "Pack of the Fallen" holds the inventory.
  - A loot window shows at most 18 items, so extra chests are spawned when needed, spaced in a ring so each one can be clicked.
  - Chests despawn after `LootDrop.ChestDuration` seconds (default 60), or as soon as they are emptied.
  - Enchants, gems, durability, crafter and spell charges are restored on the looted item.
  - Gift-wrapped items stay on the character.
- **Bots die for real.** mod-challenge-modes enforces permadeath with a kick, which does nothing to a playerbot (no client connection). A dead hardcore bot would revive and be re-killed forever, and `.playerbots bot add` would bring it straight back into the party. This module fixes that for every hardcore bot:
  - **Altbots** (characters on real accounts, mod-pbc companions included) get permadeath. They are taken out of the group, logged out, and refused at every later login.
    - `.playerbots bot add|login <names>` refuses fallen names before they log in. The other names in the list are still added.
    - Any other login path (bot autologin, `addaccount`, other modules) logs the bot straight back out.
  - **Random bots** are reset to the starting level and zone, or retired (character deleted), per `RandomBotDeathAction`.
  - A permadead bot cannot be resurrected by any means.
- **Force options.** `ForceRandomBotsHardcore` and `ForceAltBotsHardcore` make bots hardcore without the shrine.

## Requirements

- `mod-challenge-modes`
- `PlayerSettings.Enable = 1` in `worldserver.conf`
- `mod-playerbots` is optional. Without it, only the loot drop applies.

## Installation

1. Place the module in `modules/mod-hardcore-high-risk` and re-run CMake, then build.
2. The world SQL (chest templates 601000/601001) is applied by the DB updater.
3. Copy `conf/mod_hardcore_high_risk.conf.dist` to your module config folder as `mod_hardcore_high_risk.conf` and adjust.

## Reviving a fallen altbot

Delete its row with `source = 'mod-hardcore-high-risk'` in `acore_characters.character_settings`. If it was shrine-flagged, also clear the challenge-modes dead flag (index 8 of its `mod-challenge-modes` row).
