/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#ifndef MOD_HARDCORE_HIGH_RISK_H
#define MOD_HARDCORE_HIGH_RISK_H

#include "ConfigValueCache.h"
#include "ObjectGuid.h"

#if __has_include("Playerbots.h")
#define HARDCORE_HIGH_RISK_PLAYERBOTS 1
#else
#define HARDCORE_HIGH_RISK_PLAYERBOTS 0
#endif

class Player;

enum class HardcoreHighRiskConfig
{
    ENABLED,
    LOOT_DROP_ENABLED,
    LOOT_DROP_CHEST_DURATION,
    LOOT_DROP_GOLD,
    LOOT_DROP_GEAR_CHEST_ENTRY,
    LOOT_DROP_INVENTORY_CHEST_ENTRY,
    FORCE_RANDOM_BOTS_HARDCORE,
    FORCE_ALT_BOTS_HARDCORE,
    RANDOM_BOT_DEATH_ACTION,
    BOT_DEATH_ACTION_DELAY,

    NUM_CONFIGS,
};

// Indexes into this module's character_settings row (source "mod-hardcore-high-risk")
enum HardcoreHighRiskSetting : uint8
{
    SETTING_PERMADEAD                = 0, // altbot permadeath: refused at every login, never resurrected
    SETTING_PENDING_RANDOM_BOT_DEATH = 1, // random bot died and still owes its reset/retire
};

enum class RandomBotDeathAction : uint32
{
    Reset  = 0,
    Retire = 1,
};

class HardcoreHighRiskConfigData : public ConfigValueCache<HardcoreHighRiskConfig>
{
public:
    HardcoreHighRiskConfigData() : ConfigValueCache(HardcoreHighRiskConfig::NUM_CONFIGS) { }

    void BuildConfigCache() override;
};

extern HardcoreHighRiskConfigData hardcoreHighRiskConfig;

namespace HardcoreHighRisk
{
    constexpr char const* SETTING_SOURCE = "mod-hardcore-high-risk";
    constexpr char const* CHALLENGE_MODES_SOURCE = "mod-challenge-modes";

    template<class T>
    T GetConfig(HardcoreHighRiskConfig config)
    {
        return hardcoreHighRiskConfig.GetConfigValue<T>(config);
    }

    bool IsEnabled();

    // Bot classification. A bot session is any character driven by mod-playerbots (no game client).
    // "Random" bots live on the playerbots random-bot accounts (random bots and addclass bots);
    // every other bot is an altbot: a character on a real account, mod-pbc companions included.
    bool IsBotSession(Player* player);
    bool IsRandomAccountBot(Player* player);
    bool IsAltBot(Player* player);

    // Hardcore by the challenge-modes shrine flag or by one of this module's force options.
    bool IsHardcore(Player* player);

    // Dead for good: this module's permadeath flag, or challenge-modes' HARDCORE + HARDCORE_DEAD flags.
    bool IsPermaDead(Player* player);
    bool IsPermaDead(ObjectGuid guid);

    // Writes one of this module's settings both in memory and straight to the DB.
    void SetModuleSetting(Player* player, HardcoreHighRiskSetting setting, uint32 value);
}

namespace HardcoreLootDrop
{
    void OnHardcoreDeath(Player* player);
    void Update(uint32 diff);
}

namespace HardcoreBotDeath
{
    void OnHardcoreDeath(Player* player);
    void OnLogin(Player* player);
    bool CanResurrect(Player* player);
    void Update(uint32 diff);
}

void AddSC_hardcore_high_risk();
void AddSC_hardcore_loot_drop();
void AddSC_hardcore_bot_death();

#endif
