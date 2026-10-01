/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#ifndef MOD_HARDCORE_HIGH_RISK_STATE_H
#define MOD_HARDCORE_HIGH_RISK_STATE_H

#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"

// One row of `mod_hardcore_high_risk_character`. Characters without a row have every field false.
struct HardcoreCharacterState
{
    bool dead = false;            // permadeath: never resurrected again
    bool pendingBotDeath = false; // random bot died and still owes its reset or retire

    [[nodiscard]] bool IsEmpty() const { return !dead && !pendingBotDeath; }
    bool operator==(HardcoreCharacterState const&) const = default;
};

// In-memory copy of the table, loaded once at startup and written through on every change.
// Safe to use from map threads and the world thread.
namespace HardcoreState
{
    void Load();

    [[nodiscard]] HardcoreCharacterState Get(ObjectGuid::LowType guid);

    void SetDead(ObjectGuid::LowType guid, bool value);
    void SetPendingBotDeath(ObjectGuid::LowType guid, bool value);
    void Clear(ObjectGuid::LowType guid);

    // Character removed from the DB: drop its row inside the deleting transaction.
    void Erase(ObjectGuid::LowType guid, CharacterDatabaseTransaction trans);
}

#endif
