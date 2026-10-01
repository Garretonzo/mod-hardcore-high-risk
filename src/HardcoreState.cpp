/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreState.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace
{
    std::shared_mutex stateLock;
    std::unordered_map<ObjectGuid::LowType, HardcoreCharacterState> states;

    void Persist(ObjectGuid::LowType guid, HardcoreCharacterState const& state)
    {
        if (state.IsEmpty())
        {
            CharacterDatabase.Execute("DELETE FROM `mod_hardcore_high_risk_character` WHERE `guid` = {}", guid);
            return;
        }

        CharacterDatabase.Execute("REPLACE INTO `mod_hardcore_high_risk_character` "
            "(`guid`, `dead`, `pending_bot_death`) VALUES ({}, {}, {})",
            guid, uint32(state.dead), uint32(state.pendingBotDeath));
    }

    template<class Change>
    void Modify(ObjectGuid::LowType guid, Change&& change)
    {
        HardcoreCharacterState state;
        {
            std::unique_lock<std::shared_mutex> guard(stateLock);
            auto itr = states.find(guid);
            if (itr != states.end())
                state = itr->second;

            HardcoreCharacterState const before = state;
            change(state);
            if (state == before)
                return;

            if (state.IsEmpty())
                states.erase(guid);
            else
                states[guid] = state;
        }

        Persist(guid, state);
    }
}

namespace HardcoreState
{
    void Load()
    {
        std::unique_lock<std::shared_mutex> guard(stateLock);
        states.clear();

        QueryResult result = CharacterDatabase.Query("SELECT `guid`, `dead`, `pending_bot_death` "
            "FROM `mod_hardcore_high_risk_character`");
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            HardcoreCharacterState state;
            state.dead = fields[1].Get<uint8>() != 0;
            state.pendingBotDeath = fields[2].Get<uint8>() != 0;
            if (!state.IsEmpty())
                states[fields[0].Get<uint32>()] = state;
        } while (result->NextRow());

        LOG_INFO("module", "mod-hardcore-high-risk: loaded {} hardcore character state(s).", states.size());
    }

    HardcoreCharacterState Get(ObjectGuid::LowType guid)
    {
        std::shared_lock<std::shared_mutex> guard(stateLock);
        auto itr = states.find(guid);
        return itr != states.end() ? itr->second : HardcoreCharacterState();
    }

    void SetDead(ObjectGuid::LowType guid, bool value)
    {
        Modify(guid, [value](HardcoreCharacterState& state) { state.dead = value; });
    }

    void SetPendingBotDeath(ObjectGuid::LowType guid, bool value)
    {
        Modify(guid, [value](HardcoreCharacterState& state) { state.pendingBotDeath = value; });
    }

    void Clear(ObjectGuid::LowType guid)
    {
        Modify(guid, [](HardcoreCharacterState& state) { state = HardcoreCharacterState(); });
    }

    void Erase(ObjectGuid::LowType guid, CharacterDatabaseTransaction trans)
    {
        {
            std::unique_lock<std::shared_mutex> guard(stateLock);
            states.erase(guid);
        }

        trans->Append("DELETE FROM `mod_hardcore_high_risk_character` WHERE `guid` = {}", guid);
    }
}
