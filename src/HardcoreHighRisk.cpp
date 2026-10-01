/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"
#include "ChallengeModes.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "PlayerSettings.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

#if HARDCORE_HIGH_RISK_PLAYERBOTS
#include "PlayerbotAIConfig.h"
#endif

HardcoreHighRiskConfigData hardcoreHighRiskConfig;

void HardcoreHighRiskConfigData::BuildConfigCache()
{
    using Config = HardcoreHighRiskConfig;

    SetConfigValue<bool>(Config::ENABLED, "HardcoreHighRisk.Enable", true);
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

namespace
{
    bool IsChallengeModesHardcoreEnabled()
    {
        return sChallengeModes->enabled() && sChallengeModes->challengeEnabled(SETTING_HARDCORE);
    }
}

namespace HardcoreHighRisk
{
    bool IsEnabled()
    {
        return GetConfig<bool>(HardcoreHighRiskConfig::ENABLED);
    }

    bool IsBotSession(Player* player)
    {
        return player && player->GetSession() && player->GetSession()->IsBot();
    }

    bool IsRandomAccountBot([[maybe_unused]] Player* player)
    {
#if HARDCORE_HIGH_RISK_PLAYERBOTS
        return IsBotSession(player) && sPlayerbotAIConfig.IsInRandomAccountList(player->GetSession()->GetAccountId());
#else
        return false;
#endif
    }

    bool IsAltBot([[maybe_unused]] Player* player)
    {
#if HARDCORE_HIGH_RISK_PLAYERBOTS
        return IsBotSession(player) && !IsRandomAccountBot(player);
#else
        return false;
#endif
    }

    bool IsHardcore(Player* player)
    {
        if (sChallengeModes->challengeEnabledForPlayer(SETTING_HARDCORE, player))
            return true;

        if (IsRandomAccountBot(player))
            return GetConfig<bool>(HardcoreHighRiskConfig::FORCE_RANDOM_BOTS_HARDCORE);

        if (IsAltBot(player))
            return GetConfig<bool>(HardcoreHighRiskConfig::FORCE_ALT_BOTS_HARDCORE);

        return false;
    }

    bool IsPermaDead(Player* player)
    {
        if (player->GetPlayerSetting(SETTING_SOURCE, SETTING_PERMADEAD).value)
            return true;

        // challengeEnabled(HARDCORE_DEAD) always returns false, so challengeEnabledForPlayer() cannot be used for
        // the dead flag; it is read raw. Checked lazily because GetPlayerSetting() creates missing rows.
        if (!IsChallengeModesHardcoreEnabled())
            return false;

        return player->GetPlayerSetting(CHALLENGE_MODES_SOURCE, SETTING_HARDCORE).value
            && player->GetPlayerSetting(CHALLENGE_MODES_SOURCE, HARDCORE_DEAD).value;
    }

    bool IsPermaDead(ObjectGuid guid)
    {
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_SETTINGS);
        stmt->SetData(0, guid.GetCounter());
        PreparedQueryResult result = CharacterDatabase.Query(stmt);
        if (!result)
            return false;

        auto valueAt = [](PlayerSettingVector const& settings, uint32 index) -> uint32
        {
            return index < settings.size() ? settings[index].value : 0;
        };

        do
        {
            Field* fields = result->Fetch();
            std::string const source = fields[0].Get<std::string>();
            PlayerSettingVector const settings = PlayerSettingsStore::ParseSettingsData(fields[1].Get<std::string>());

            if (source == SETTING_SOURCE && valueAt(settings, SETTING_PERMADEAD))
                return true;

            if (source == CHALLENGE_MODES_SOURCE && IsChallengeModesHardcoreEnabled()
                && valueAt(settings, SETTING_HARDCORE) && valueAt(settings, HARDCORE_DEAD))
                return true;
        } while (result->NextRow());

        return false;
    }

    void SetModuleSetting(Player* player, HardcoreHighRiskSetting setting, uint32 value)
    {
        player->UpdatePlayerSetting(SETTING_SOURCE, setting, value);

        if (!sWorld->getBoolConfig(CONFIG_PLAYER_SETTINGS_ENABLED))
            return;

        // Persist now: a crash or a bot logout must not lose a permadeath. Rebuilt from memory so no read is needed.
        PlayerSettingVector settings;
        settings.emplace_back(player->GetPlayerSetting(SETTING_SOURCE, SETTING_PERMADEAD).value);
        settings.emplace_back(player->GetPlayerSetting(SETTING_SOURCE, SETTING_PENDING_RANDOM_BOT_DEATH).value);
        ObjectGuid::LowType const lowGuid = player->GetGUID().GetCounter();
        CharacterDatabase.Execute(PlayerSettingsStore::PrepareReplaceStatement(lowGuid, SETTING_SOURCE, settings));
    }
}

class HardcoreHighRiskPlayerScript : public PlayerScript
{
public:
    HardcoreHighRiskPlayerScript() : PlayerScript("HardcoreHighRiskPlayerScript", {
        PLAYERHOOK_ON_PLAYER_JUST_DIED,
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_CAN_RESURRECT
    }) { }

    // One hook drives both features so the loot always drops before a bot reset wipes the inventory.
    void OnPlayerJustDied(Player* player) override
    {
        if (!HardcoreHighRisk::IsEnabled() || !HardcoreHighRisk::IsHardcore(player))
            return;

        HardcoreLootDrop::OnHardcoreDeath(player);
        HardcoreBotDeath::OnHardcoreDeath(player);
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!HardcoreHighRisk::IsEnabled())
            return;

        HardcoreBotDeath::OnLogin(player);
    }

    bool OnPlayerCanResurrect(Player* player) override
    {
        if (!HardcoreHighRisk::IsEnabled())
            return true;

        return HardcoreBotDeath::CanResurrect(player);
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

    void OnStartup() override
    {
        if (HardcoreHighRisk::IsEnabled() && !sWorld->getBoolConfig(CONFIG_PLAYER_SETTINGS_ENABLED))
            LOG_ERROR("module", "mod-hardcore-high-risk: PlayerSettings.Enable is off, so hardcore flags and "
                "permadeath cannot be stored.");
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
