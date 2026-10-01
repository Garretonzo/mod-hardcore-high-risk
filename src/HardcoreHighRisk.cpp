/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"
#include "Chat.h"
#include "Corpse.h"
#include "Group.h"
#include "HardcoreState.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldSession.h"

#if HARDCORE_HIGH_RISK_PLAYERBOTS
#include "PlayerbotAIConfig.h"
#endif

HardcoreHighRiskConfigData hardcoreHighRiskConfig;

void HardcoreHighRiskConfigData::BuildConfigCache()
{
    using Config = HardcoreHighRiskConfig;

    SetConfigValue<bool>(Config::ENABLED, "HardcoreHighRisk.Enable", true);
    SetConfigValue<bool>(Config::HARDCORE_PLAYERS, "HardcoreHighRisk.Players", true);
    SetConfigValue<bool>(Config::HARDCORE_ALT_BOTS, "HardcoreHighRisk.AltBots", true);
    SetConfigValue<bool>(Config::HARDCORE_RANDOM_BOTS, "HardcoreHighRisk.RandomBots", true);
    SetConfigValue<bool>(Config::LOOT_DROP_ENABLED, "HardcoreHighRisk.LootDrop.Enable", true);
    SetConfigValue<uint32>(Config::LOOT_DROP_CHEST_DURATION, "HardcoreHighRisk.LootDrop.ChestDuration", 60,
        Reloadable::Yes, [](uint32 const& value) { return value > 0; }, "> 0");
    SetConfigValue<bool>(Config::LOOT_DROP_GOLD, "HardcoreHighRisk.LootDrop.DropGold", true);
    SetConfigValue<uint32>(Config::LOOT_DROP_GEAR_CHEST_ENTRY, "HardcoreHighRisk.LootDrop.GearChestEntry", 601000);
    SetConfigValue<uint32>(Config::LOOT_DROP_INVENTORY_CHEST_ENTRY, "HardcoreHighRisk.LootDrop.InventoryChestEntry",
        601001);
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
        if (!IsEnabled())
            return false;

        if (IsRandomAccountBot(player))
            return GetConfig<bool>(HardcoreHighRiskConfig::HARDCORE_RANDOM_BOTS);

        if (IsAltBot(player))
            return GetConfig<bool>(HardcoreHighRiskConfig::HARDCORE_ALT_BOTS);

        return GetConfig<bool>(HardcoreHighRiskConfig::HARDCORE_PLAYERS);
    }

    bool IsPermaDead(Player const* player)
    {
        return HardcoreState::Get(player->GetGUID().GetCounter()).dead;
    }
}

namespace
{
    constexpr Milliseconds SWEEP_INTERVAL = 5s;

    // No soulstone/Reincarnation button and no pending resurrect offer for the fallen.
    void ClearResurrectionOptions(Player* player)
    {
        if (player->GetUInt32Value(PLAYER_SELF_RES_SPELL))
            player->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);

        if (player->isResurrectRequested())
            player->clearResurrectRequestData();
    }

    // The fallen character remembers how it died; altbot companions in its group remember the loss.
    void RecordDeathInPbc(Player* fallen, HardcoreDeathContext::DeathStory const& story)
    {
        if (HardcoreHighRisk::IsAltBot(fallen))
            HardcorePbc::AddMemory(fallen->GetGUID().GetCounter(), story.ForSelf());

        Group* group = fallen->GetGroup();
        if (!group)
            return;

        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && member != fallen && HardcoreHighRisk::IsAltBot(member))
                HardcorePbc::AddMemory(member->GetGUID().GetCounter(), story.ForCompanion(member->GetName()));
        }
    }

    // Keeps every online fallen character in its proper state (logins and bot strategy resets undo it).
    void SweepFallen()
    {
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !player->IsInWorld() || !HardcoreHighRisk::IsPermaDead(player))
                continue;

            ClearResurrectionOptions(player);

            if (HardcoreHighRisk::IsAltBot(player))
                HardcoreBotDeath::MaintainAltBotGhost(player);
        }
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
        if (!HardcoreHighRisk::IsHardcore(player))
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
        ClearResurrectionOptions(player);
        RecordDeathInPbc(player, HardcoreDeathContext::Take(player));

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

    // Catch-all for the spirit healer, corpse reclaim, battlegrounds and .revive: they all go through
    // Player::ResurrectPlayer, which asks this hook first. Resurrect spells are refused earlier, at cast time.
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

// Refuses resurrection spells (Resurrection, Rebirth, Redemption, Ancestral Spirit, soulstones, Reincarnation)
// aimed at the fallen before any resurrect offer exists. Otherwise Player::ResurectUsingRequestData would restore
// health and mana even though ResurrectPlayer refuses, leaving a body that looks alive but is not.
class HardcoreHighRiskSpellScript : public AllSpellScript
{
public:
    HardcoreHighRiskSpellScript() : AllSpellScript("HardcoreHighRiskSpellScript", {
        ALLSPELLHOOK_ON_SPELL_CHECK_CAST
    }) { }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (res != SPELL_CAST_OK || !HardcoreHighRisk::IsEnabled())
            return;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        bool const selfResurrect = spellInfo->HasEffect(SPELL_EFFECT_SELF_RESURRECT);
        if (!selfResurrect && !spellInfo->HasEffect(SPELL_EFFECT_RESURRECT)
            && !spellInfo->HasEffect(SPELL_EFFECT_RESURRECT_NEW))
            return;

        ObjectGuid target;
        if (selfResurrect)
            target = spell->GetCaster() ? spell->GetCaster()->GetGUID() : ObjectGuid::Empty;
        else if (Unit* unitTarget = spell->m_targets.GetUnitTarget())
            target = unitTarget->GetGUID();
        else if (Corpse* corpse = spell->m_targets.GetCorpseTarget())
            target = corpse->GetOwnerGUID();

        if (target.IsPlayer() && HardcoreState::Get(target.GetCounter()).dead)
            res = SPELL_FAILED_TARGET_CANNOT_BE_RESURRECTED;
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

    // Before the world loop processes any login.
    void OnStartup() override
    {
        HardcoreState::Load();
    }

    // Runs on the world thread after all maps have updated.
    void OnUpdate(uint32 diff) override
    {
        HardcoreLootDrop::Update(diff);
        HardcoreBotDeath::Update(diff);

        sweepTimer += Milliseconds(diff);
        if (sweepTimer < SWEEP_INTERVAL)
            return;

        sweepTimer = 0ms;
        if (HardcoreHighRisk::IsEnabled())
            SweepFallen();
    }

private:
    Milliseconds sweepTimer = 0ms;
};

void AddSC_hardcore_high_risk()
{
    new HardcoreHighRiskPlayerScript();
    new HardcoreHighRiskSpellScript();
    new HardcoreHighRiskWorldScript();
}
