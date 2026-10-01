/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"
#include "Chat.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "HardcoreState.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "World.h"

namespace
{
    enum HardcoreShrineData
    {
        NPC_TEXT_HARDCORE_SHRINE = 601000,
        ACTION_EMBRACE_HARDCORE  = 1,
    };

    bool IsShrineActive()
    {
        using HardcoreHighRisk::GetConfig;
        return HardcoreHighRisk::IsEnabled() && GetConfig<bool>(HardcoreHighRiskConfig::SHRINE_ENABLED);
    }

    // Only a character at the very start of its journey may choose hardcore, and only once.
    bool CanEmbraceHardcore(Player const* player)
    {
        if (HardcoreState::Get(player->GetGUID().GetCounter()).hardcore)
            return false;

        uint32 const maxLevel = player->getClass() == CLASS_DEATH_KNIGHT
            ? sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL)
            : sWorld->getIntConfig(CONFIG_START_PLAYER_LEVEL);
        return player->GetLevel() <= maxLevel;
    }
}

class go_hardcore_high_risk_shrine : public GameObjectScript
{
public:
    go_hardcore_high_risk_shrine() : GameObjectScript("go_hardcore_high_risk_shrine") { }

    struct go_hardcore_high_risk_shrineAI : public GameObjectAI
    {
        explicit go_hardcore_high_risk_shrineAI(GameObject* go) : GameObjectAI(go) { }

        bool CanBeSeen(Player const* seer) override
        {
            return IsShrineActive() && CanEmbraceHardcore(seer);
        }
    };

    bool OnGossipHello(Player* player, GameObject* go) override
    {
        if (IsShrineActive() && CanEmbraceHardcore(player))
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Embrace the hardcore challenge.", GOSSIP_SENDER_MAIN,
                ACTION_EMBRACE_HARDCORE, "Death will be permanent. Everything you carry drops where you fall. "
                "This cannot be undone.", 0, false);
        }

        SendGossipMenuFor(player, NPC_TEXT_HARDCORE_SHRINE, go->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, GameObject* go, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);

        if (action != ACTION_EMBRACE_HARDCORE || !IsShrineActive() || !CanEmbraceHardcore(player))
            return true;

        HardcoreState::SetHardcore(player->GetGUID().GetCounter(), true);
        ChatHandler(player->GetSession()).SendSysMessage("You are now hardcore. Death will be permanent.");
        go->DestroyForPlayer(player);
        return true;
    }

    GameObjectAI* GetAI(GameObject* go) const override
    {
        return new go_hardcore_high_risk_shrineAI(go);
    }
};

void AddSC_hardcore_shrine()
{
    new go_hardcore_high_risk_shrine();
}
