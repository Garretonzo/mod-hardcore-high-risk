# mod-hardcore-high-risk

A standalone hardcore mode for AzerothCore (3.3.5a), for players and playerbots alike.

## Features

- **Opt in at the Shrine of the Hardcore.** One shrine stands in each starting zone.
  - Only a character still at its starting level (death knights: theirs) that is not hardcore yet can see it.
  - Choosing hardcore asks for confirmation, and it cannot be undone.
- **Permanent death.** A fallen hardcore character becomes a permanent ghost. It can still log in, run around and chat, so it can say goodbye, but nothing resurrects it: no spirit healer, corpse run, spell, battleground or GM command.
- **Everything drops.** On death, all equipped gear, bags, bag contents, the keyring and (optionally) gold drop into chests next to the body, and anyone can loot them.
  - "Gear of the Fallen" holds equipped gear and gold, and "Pack of the Fallen" holds the inventory.
  - A loot window shows at most 18 items, so extra chests spawn when needed, spaced in a ring so each one can be clicked.
  - Chests despawn after `LootDrop.ChestDuration` seconds (default 60), or as soon as they are emptied.
  - Enchants, gems, durability, crafter and spell charges are restored on the looted item.
  - Gift-wrapped items stay on the character.
- **Playerbots** (optional, needs mod-playerbots):
  - **Altbots** (characters on real accounts, mod-pbc companions included) die like real players. They become ghosts that stay in the group, follow and talk, but never revive, and their master gets a "fallen" notice. `.playerbots bot add` brings them back as ghosts.
  - **Random bots** are reset to the starting level and zone, or retired (character deleted), per `RandomBotDeathAction`.
  - `ForceRandomBotsHardcore` and `ForceAltBotsHardcore` make bots hardcore without the shrine.

## Requirements

- AzerothCore with module support.
- `mod-playerbots` is optional. Without it, the bot features are simply off.

## Installation

1. Place the module in `modules/mod-hardcore-high-risk`, re-run CMake and build.
2. The SQL is applied by the DB updater:
   - world: the chest templates 601000/601001, the shrine template 254605 with its 9 starting-zone spawns, and `npc_text` 601000;
   - characters: the `mod_hardcore_high_risk_character` table.
3. Copy `conf/mod_hardcore_high_risk.conf.dist` to your module config folder as `mod_hardcore_high_risk.conf` and adjust it.

## Bringing a character back

Delete its row in `acore_characters.mod_hardcore_high_risk_character`. This also removes its hardcore status:

```sql
DELETE FROM mod_hardcore_high_risk_character WHERE guid = <character guid>;
```

Then restart the worldserver. The state is loaded at startup.
