/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#ifndef MOD_HARDCORE_HIGH_RISK_H
#define MOD_HARDCORE_HIGH_RISK_H

#include "ConfigValueCache.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

#if __has_include("Playerbots.h")
#define HARDCORE_HIGH_RISK_PLAYERBOTS 1
#else
#define HARDCORE_HIGH_RISK_PLAYERBOTS 0
#endif

class Player;
class Unit;

enum class HardcoreHighRiskConfig
{
    ENABLED,
    HARDCORE_PLAYERS,
    HARDCORE_ALT_BOTS,
    HARDCORE_RANDOM_BOTS,
    LOOT_DROP_ENABLED,
    LOOT_DROP_CHEST_DURATION,
    LOOT_DROP_GOLD,
    LOOT_DROP_GEAR_CHEST_ENTRY,
    LOOT_DROP_INVENTORY_CHEST_ENTRY,
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

    // Populations. A bot session is any character driven by mod-playerbots (no game client).
    // "Random" bots live on the playerbots random-bot accounts (random bots and addclass bots);
    // every other bot is an altbot: a character on a real account, mod-pbc companions included.
    bool IsBotSession(Player const* player);
    bool IsRandomAccountBot(Player const* player);
    bool IsAltBot(Player const* player);

    // Hardcore by config: the whole realm, per population.
    bool IsHardcore(Player const* player);

    // Fallen for good: never resurrected again.
    bool IsPermaDead(Player const* player);
}

namespace HardcoreLootDrop
{
    void OnHardcoreDeath(Player* player);
    void Update(uint32 diff);
}

// How a hardcore character died, captured at the killing blow and turned into narration.
namespace HardcoreDeathContext
{
    class DeathStory
    {
    public:
        // Second person, for the fallen character itself.
        [[nodiscard]] std::string ForSelf() const;
        // Third person, for a companion; the companion appears as "you" among those present.
        [[nodiscard]] std::string ForCompanion(std::string const& companionName) const;

        std::string name;
        std::string date;
        std::string place;
        uint8 level = 0;
        std::string cause;                 // "slain by a level 10 elite Defias Overseer", "by drowning", ...
        std::string doing;                 // " while fighting ... for the quest ...", may be empty
        std::vector<std::string> present;  // group members close by
    };

    // Called at the killing blow (inside Unit::Kill), before OnPlayerJustDied.
    void Capture(Player* victim, Unit* killer);
    // Takes the captured story, or builds a minimal one when nothing was captured.
    DeathStory Take(Player* victim);
}

// Writes permanent memories into mod-pbc; no-op without mod-pbc.
namespace HardcorePbc
{
    void AddMemory(ObjectGuid::LowType characterGuid, std::string const& text);
}

// Random-bot reset/retire and altbot ghosts; empty without mod-playerbots.
namespace HardcoreBotDeath
{
    void OnRandomBotDeath(Player* bot);
    void OnRandomBotLogin(Player* bot);
    void OnAltBotFallen(Player* bot);
    // Keeps a fallen altbot a following ghost (re-applied after logins, which reset bot strategies).
    void MaintainAltBotGhost(Player* bot);
    void Update(uint32 diff);
}

void AddSC_hardcore_high_risk();
void AddSC_hardcore_loot_drop();
void AddSC_hardcore_death_context();

#endif
