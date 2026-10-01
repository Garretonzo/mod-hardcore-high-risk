/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"
#include "Chat.h"
#include "HardcoreState.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#if HARDCORE_HIGH_RISK_PLAYERBOTS
#include "PlayerbotAIConfig.h"
#endif

HardcoreHighRiskConfigData hardcoreHighRiskConfig;

void HardcoreHighRiskConfigData::BuildConfigCache()
{
    using Config = HardcoreHighRiskConfig;

    SetConfigValue<bool>(Config::ENABLED, "HardcoreHighRisk.Enable", true);
    SetConfigValue<bool>(Config::SHRINE_ENABLED, "HardcoreHighRisk.Shrine.Enable", true);
    SetConfigValue<bool>(Config::LOOT_DROP_ENABLED, "HardcoreHighRisk.LootDrop.Enable", true);
    SetConfigValue<uint32>(Config::LOOT_DROP_CHEST_DURATION, "HardcoreHighRisk.LootDrop.ChestDuration", 60,
        Reloadable::Yes, [](uint32 const& value) { return value > 0; }, "> 0");
    SetConfigValue<bool>(Config::LOOT_DROP_GOLD, "HardcoreHighRisk.LootDrop.DropGold", true);
    SetConfigValue<uint32>(Config::LOOT_DROP_GEAR_CHEST_ENTRY, "HardcoreHighRisk.LootDrop.GearChestEntry", 601000);
    SetConfigValue<uint32>(Config::LOOT_DROP_INVENTORY_CHEST_ENTRY, "HardcoreHighRisk.LootDrop.InventoryChestEntry",
        601001);
    SetConfigValue<bool>(Config::FORCE_RANDOM_BOTS_HARDCORE, "HardcoreHighRisk.ForceRandomBotsHardcore", false);
    SetConfigValue<bool>(Config::FORCE_ALT_BOTS_HARDCORE, "HardcoreHighRisk.ForceAltBotsHardcore", false);
    SetConfigValue<uint32>(Config::RANDOM_BOT_DEATH_ACTION, "HardcoreHighRisk.RandomBotDeathAction", 0,
        Reloadable::Yes, [](uint32 const& value) { return value <= uint32(RandomBotDeathAction::Retire); }, "0 or 1");
    SetConfigValue<uint32>(Config::BOT_DEATH_ACTION_DELAY, "HardcoreHighRisk.BotDeathActionDelay", 5);
}

namespace HardcoreHighRisk
{
    bool IsEnabled()
    {
        return GetConfig<bool>(HardcoreHighRiskConfig::ENABLED);
    }

    bool IsBotSession(Player const* player)
    {
        return player && player->GetSession() && player->GetSession()->IsBot();
    }

    bool IsRandomAccountBot([[maybe_unused]] Player const* player)
    {
#if HARDCORE_HIGH_RISK_PLAYERBOTS
        return IsBotSession(player) && sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId());
#else
        return false;
#endif
    }

    bool IsAltBot([[maybe_unused]] Player const* player)
    {
#if HARDCORE_HIGH_RISK_PLAYERBOTS
        return IsBotSession(player) && !IsRandomAccountBot(player);
#else
        return false;
#endif
    }

    bool IsHardcore(Player const* player)
    {
        if (HardcoreState::Get(player->GetGUID().GetCounter()).hardcore)
            return true;

        if (IsRandomAccountBot(player))
            return GetConfig<bool>(HardcoreHighRiskConfig::FORCE_RANDOM_BOTS_HARDCORE);

        if (IsAltBot(player))
            return GetConfig<bool>(HardcoreHighRiskConfig::FORCE_ALT_BOTS_HARDCORE);

        return false;
    }

    bool IsPermaDead(Player const* player)
    {
        return HardcoreState::Get(player->GetGUID().GetCounter()).dead;
    }
}

class HardcoreHighRiskPlayerScript : public PlayerScript
{
public:
    HardcoreHighRiskPlayerScript() : PlayerScript("HardcoreHighRiskPlayerScript", {
        PLAYERHOOK_ON_PLAYER_JUST_DIED,
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_CAN_RESURRECT,
        PLAYERHOOK_ON_DELETE_FROM_DB
    }) { }

    // One hook drives everything so the loot always drops before a bot reset wipes the inventory.
    void OnPlayerJustDied(Player* player) override
    {
        if (!HardcoreHighRisk::IsEnabled() || !HardcoreHighRisk::IsHardcore(player))
            return;

        HardcoreLootDrop::OnHardcoreDeath(player);

        // Random bots are recycled (reset or retired) instead of staying dead.
        if (HardcoreHighRisk::IsRandomAccountBot(player))
        {
            HardcoreBotDeath::OnRandomBotDeath(player);
            return;
        }

        // Repeat deaths (e.g. the re-kill at login) change nothing.
        if (HardcoreHighRisk::IsPermaDead(player))
            return;

        // Real players and altbots stay on as ghosts: they can still log in, move and talk, but never resurrect.
        HardcoreState::SetDead(player->GetGUID().GetCounter(), true);

        if (HardcoreHighRisk::IsBotSession(player))
            HardcoreBotDeath::OnAltBotFallen(player);
        else
            ChatHandler(player->GetSession()).SendSysMessage("You have fallen. In hardcore, death is permanent.");
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!HardcoreHighRisk::IsEnabled())
            return;

        if (HardcoreHighRisk::IsRandomAccountBot(player))
        {
            HardcoreBotDeath::OnRandomBotLogin(player);
            return;
        }

        // Loaded alive despite the permadeath (e.g. a crash before the save): put the body back where it belongs.
        if (HardcoreHighRisk::IsPermaDead(player) && player->IsAlive())
            player->KillPlayer();
    }

    // Blocks every resurrection path (spirit healer, corpse, spells, battlegrounds, .revive): all go through
    // Player::ResurrectPlayer, which asks this hook first.
    bool OnPlayerCanResurrect(Player* player) override
    {
        if (!HardcoreHighRisk::IsEnabled() || !HardcoreHighRisk::IsPermaDead(player))
            return true;

        // Bots retry on their own; only real players get told.
        if (!HardcoreHighRisk::IsBotSession(player))
            ChatHandler(player->GetSession()).SendSysMessage("In hardcore, the fallen are never resurrected.");

        return false;
    }

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        HardcoreState::Erase(guid, trans);
    }
};

class HardcoreHighRiskWorldScript : public WorldScript
{
public:
    HardcoreHighRiskWorldScript() : WorldScript("HardcoreHighRiskWorldScript", {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnBeforeConfigLoad(bool reload) override
    {
        hardcoreHighRiskConfig.Initialize(reload);
    }

    // Before anyone can log in.
    void OnStartup() override
    {
        HardcoreState::Load();
    }

    // Runs on the world thread after all maps have updated.
    void OnUpdate(uint32 diff) override
    {
        HardcoreLootDrop::Update(diff);
        HardcoreBotDeath::Update(diff);
    }
};

void AddSC_hardcore_high_risk()
{
    new HardcoreHighRiskPlayerScript();
    new HardcoreHighRiskWorldScript();
}
