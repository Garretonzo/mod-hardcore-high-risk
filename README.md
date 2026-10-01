# mod-hardcore-high-risk

Turns an AzerothCore (3.3.5a) realm into a hardcore, high-risk realm for everyone on it: real players and playerbots alike.

## How it works

- **Everyone is hardcore.** With the module enabled, every character in the enabled populations is hardcore: players (game masters included), altbots and random bots. There is nothing to opt into, and only the config decides who is affected.
- **Permanent death.** A fallen player or altbot becomes a permanent ghost. It can still log in, move around and chat, so it can say goodbye, but it can never be resurrected.
  - Resurrection spells (Resurrection, Rebirth, Redemption, Ancestral Spirit), soulstones and Reincarnation are refused at cast time.
  - The spirit healer, corpse reclaim, battleground resurrection and `.revive` are refused too.
- **Everything drops.** On death, all equipped gear, bags, bag contents, the keyring and (optionally) gold drop into chests next to the body, and anyone can loot them.
  - "Gear of the Fallen" holds equipped gear and gold, and "Pack of the Fallen" holds the inventory.
  - A loot window shows at most 18 items, so extra chests spawn when needed, spaced in a ring so each one can be clicked.
  - Chests despawn after `LootDrop.ChestDuration` seconds (default 60), or as soon as they are emptied.
  - Enchants, gems, durability, crafter and spell charges are restored on the looted item.
  - Gift-wrapped items stay on the character.
- **Playerbots** (optional, needs mod-playerbots):
  - **Altbots** (characters on real accounts, mod-pbc companions included) release at once into a ghost where they fell and keep following their master. Their master gets a "fallen" notice.
  - **Random bots** are reset to the starting level and zone, or retired (character deleted), per `RandomBotDeathAction`.
- **mod-pbc companions remember** (optional, needs mod-pbc). Each death gets a permanent, top-importance memory written from the killing blow: where, what killed them, what they were fighting, the quest they were on, and who was with them.
  - The fallen companion gets it in its own voice, so it talks as a ghost who knows how it died.
  - Every companion in the group gets it as the loss of a friend, and that includes when you die.

## Requirements

- AzerothCore with module support.
- `mod-playerbots` and `mod-pbc` are optional. Without them, the bot and companion features are simply off.

## Installation

1. Place the module in `modules/mod-hardcore-high-risk`, re-run CMake and build.
2. The SQL is applied by the DB updater:
   - world: the chest templates 601000/601001, plus removal of the former Shrine of Challenge / Shrine of the Hardcore (gameobject 254605);
   - characters: the `mod_hardcore_high_risk_character` table.
3. Copy `conf/mod_hardcore_high_risk.conf.dist` to your module config folder as `mod_hardcore_high_risk.conf` and adjust it.

## Bringing a character back

Delete its row in `acore_characters.mod_hardcore_high_risk_character`, then restart the worldserver. The state is loaded at startup.

```sql
DELETE FROM mod_hardcore_high_risk_character WHERE guid = <character guid>;
```
