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
    SHRINE_ENABLED,
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
    template<class T>
    T GetConfig(HardcoreHighRiskConfig config)
    {
        return hardcoreHighRiskConfig.GetConfigValue<T>(config);
    }

    bool IsEnabled();

    // Bot classification. A bot session is any character driven by mod-playerbots (no game client).
    // "Random" bots live on the playerbots random-bot accounts (random bots and addclass bots);
    // every other bot is an altbot: a character on a real account, mod-pbc companions included.
    bool IsBotSession(Player const* player);
    bool IsRandomAccountBot(Player const* player);
    bool IsAltBot(Player const* player);

    // Hardcore by the shrine or by one of the force options.
    bool IsHardcore(Player const* player);

    // Fallen for good: never resurrected again.
    bool IsPermaDead(Player const* player);
}

namespace HardcoreLootDrop
{
    void OnHardcoreDeath(Player* player);
    void Update(uint32 diff);
}

// Random-bot reset/retire and the altbot master notice; empty without mod-playerbots.
namespace HardcoreBotDeath
{
    void OnRandomBotDeath(Player* bot);
    void OnRandomBotLogin(Player* bot);
    void OnAltBotFallen(Player* bot);
    void Update(uint32 diff);
}

void AddSC_hardcore_high_risk();
void AddSC_hardcore_loot_drop();
void AddSC_hardcore_shrine();

#endif
