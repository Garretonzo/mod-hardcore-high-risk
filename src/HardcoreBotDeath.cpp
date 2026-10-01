/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"
#include "Player.h"

#if HARDCORE_HIGH_RISK_PLAYERBOTS
#include "CalendarMgr.h"
#include "Chat.h"
#include "Group.h"
#include "HardcoreState.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include <mutex>
#include <unordered_map>

namespace
{
    enum class PendingAction : uint8
    {
        RandomBotDeath,  // random-account bot died: reset or retire per config
        DeleteCharacter, // retire, second step: delete once the bot is offline
        AltBotGhost,     // fallen altbot: release into a ghost at its body and follow the master
    };

    // A fallen altbot releases this long after dying (never inside the KillPlayer hook itself).
    constexpr Milliseconds ALT_BOT_GHOST_DELAY = 1s;

    struct PendingBotAction
    {
        PendingAction action;
        Milliseconds delay;
        Milliseconds age;
        uint32 accountId;
    };

    // Bots that are not registered with a holder yet are retried; give up after this long.
    constexpr Milliseconds PENDING_TIMEOUT = 2min;
    constexpr Milliseconds RETRY_DELAY = 1s;

    std::mutex pendingLock;
    std::unordered_map<ObjectGuid, PendingBotAction> pendingActions;

    void QueueAction(ObjectGuid guid, PendingAction action, Milliseconds delay, uint32 accountId = 0)
    {
        std::lock_guard<std::mutex> guard(pendingLock);
        auto itr = pendingActions.find(guid);
        // A retire that already logged the bot out must still finish its delete.
        if (itr != pendingActions.end() && itr->second.action == PendingAction::DeleteCharacter)
            return;

        pendingActions[guid] = { action, delay, 0ms, accountId };
    }

    PlayerbotHolder* FindHolder(Player* bot)
    {
        ObjectGuid const guid = bot->GetGUID();
        if (sRandomPlayerbotMgr.GetPlayerBot(guid))
            return &sRandomPlayerbotMgr;

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            if (Player* master = botAI->GetMaster())
                if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(master))
                    if (mgr->GetPlayerBot(guid))
                        return mgr;

        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
            if (Player* player = session->GetPlayer())
                if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player))
                    if (mgr->GetPlayerBot(guid))
                        return mgr;

        return nullptr;
    }

    // Returns true once the bot is offline.
    bool LogoutBot(ObjectGuid guid)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!bot)
            return true;

        // Not registered with a holder until OnBotLogin runs; try again next tick.
        PlayerbotHolder* holder = FindHolder(bot);
        if (!holder)
            return false;

        if (Group* group = bot->GetGroup())
            group->RemoveMember(guid);

        holder->LogoutPlayerBot(guid);
        return !ObjectAccessor::FindConnectedPlayer(guid);
    }

    bool ResetRandomBot(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || !bot->IsInWorld() || bot->IsBeingTeleported())
            return false;

        // A fresh start: no longer hardcore, dead or pending (force options can make it hardcore again).
        HardcoreState::Clear(bot->GetGUID().GetCounter());

        if (!bot->IsAlive())
        {
            bot->ResurrectPlayer(1.0f);
            bot->SpawnCorpseBones();
        }

        if (bot->GetGroup())
            botAI->LeaveOrDisbandGroup();

        // Mirrors RandomPlayerbotMgr::RandomizeFirst, pinned to the starting level.
        uint32 const level = bot->getClass() == CLASS_DEATH_KNIGHT
            ? sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL)
            : sWorld->getIntConfig(CONFIG_START_PLAYER_LEVEL);
        sRandomPlayerbotMgr.SetValue(bot, "level", level);
        PlayerbotFactory factory(bot, level);
        factory.Randomize(false);
        botAI->Reset(true);

        if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(bot->getRace(), bot->getClass()))
            bot->TeleportTo(info->mapId, info->positionX, info->positionY, info->positionZ, info->orientation);

        bot->SaveToDB(false, false);
        LOG_INFO("module", "mod-hardcore-high-risk: random bot {} died in hardcore and was reset to level {}.",
            bot->GetName(), level);
        return true;
    }

    bool RetireRandomBot(Player* bot)
    {
        if (!GET_PLAYERBOT_AI(bot))
            return false;

        ObjectGuid const guid = bot->GetGUID();
        uint32 const accountId = bot->GetSession()->GetAccountId();
        LOG_INFO("module", "mod-hardcore-high-risk: random bot {} died in hardcore and is retired (character deleted).",
            bot->GetName());

        // Drops its random-bot rows and logs it out when the random bot manager holds it. bot is invalid afterwards.
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
            sRandomPlayerbotMgr.Remove(bot);

        // Addclass bots are held by their master instead.
        LogoutBot(guid);

        QueueAction(guid, PendingAction::DeleteCharacter, 0ms, accountId);
        return true;
    }

    bool ProcessRandomBotDeath(ObjectGuid guid)
    {
        // Offline: the pending flag is stored, so the action runs at its next login.
        Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!bot)
            return true;

        auto const action = RandomBotDeathAction(
            HardcoreHighRisk::GetConfig<uint32>(HardcoreHighRiskConfig::RANDOM_BOT_DEATH_ACTION));
        return action == RandomBotDeathAction::Retire ? RetireRandomBot(bot) : ResetRandomBot(bot);
    }

    bool DeleteCharacter(ObjectGuid guid, uint32 accountId)
    {
        if (ObjectAccessor::FindConnectedPlayer(guid))
            return false;

        // Same steps as WorldSession::HandleCharDeleteOpcode. DeleteFromDB also drops the module's state row.
        sScriptMgr->OnPlayerDelete(guid, accountId);
        sCalendarMgr->RemoveAllPlayerEventsAndInvites(guid);
        Player::DeleteFromDB(guid.GetCounter(), accountId, true, true);
        sWorld->UpdateRealmCharCount(accountId);
        return true;
    }

    bool ProcessAction(ObjectGuid guid, PendingBotAction const& pending)
    {
        switch (pending.action)
        {
            case PendingAction::RandomBotDeath:
                return ProcessRandomBotDeath(guid);
            case PendingAction::DeleteCharacter:
                return DeleteCharacter(guid, pending.accountId);
            case PendingAction::AltBotGhost:
            {
                // Offline: the sweep finishes the job after its next login.
                Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
                if (!bot)
                    return true;

                HardcoreBotDeath::MaintainAltBotGhost(bot);
                return bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
            }
        }

        return true;
    }
}

namespace HardcoreBotDeath
{
    void OnRandomBotDeath(Player* bot)
    {
        HardcoreState::SetPendingBotDeath(bot->GetGUID().GetCounter(), true);

        Milliseconds const delay = Seconds(
            HardcoreHighRisk::GetConfig<uint32>(HardcoreHighRiskConfig::BOT_DEATH_ACTION_DELAY));
        QueueAction(bot->GetGUID(), PendingAction::RandomBotDeath, delay);
    }

    // The bot is not registered with its holder yet, and HandlePlayerBotLoginCallback still uses it after the
    // login hook, so it is only queued here; Update() acts once playerbots has finished the login.
    void OnRandomBotLogin(Player* bot)
    {
        if (HardcoreState::Get(bot->GetGUID().GetCounter()).pendingBotDeath)
            QueueAction(bot->GetGUID(), PendingAction::RandomBotDeath, 0ms);
    }

    void OnAltBotFallen(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        Player* master = botAI ? botAI->GetMaster() : nullptr;
        if (master && master != bot && master->GetSession())
        {
            ChatHandler(master->GetSession()).SendSysMessage(
                Acore::StringFormat("{} has fallen. They remain as a ghost.", bot->GetName()));
        }

        QueueAction(bot->GetGUID(), PendingAction::AltBotGhost, ALT_BOT_GHOST_DELAY);
    }

    void MaintainAltBotGhost(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || !bot->IsInWorld() || bot->IsBeingTeleported())
            return;

        // A ghost right where it fell (no graveyard teleport), so it can stay with its group.
        if (bot->getDeathState() == DeathState::Corpse && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            bot->BuildPlayerRepop();

        // Without the "dead" strategy it no longer runs to its corpse, retries revives or accepts resurrects;
        // the dead engine's "follow" keeps it with the master. Logins reset strategies, hence the re-check.
        if (botAI->HasStrategy("dead", BOT_STATE_DEAD))
            botAI->ChangeStrategy("-dead,-stay,+follow", BOT_STATE_DEAD);
    }

    void Update(uint32 diff)
    {
        Milliseconds const elapsed(diff);
        std::vector<std::pair<ObjectGuid, PendingBotAction>> due;
        {
            std::lock_guard<std::mutex> guard(pendingLock);
            for (auto itr = pendingActions.begin(); itr != pendingActions.end();)
            {
                PendingBotAction& pending = itr->second;
                pending.age += elapsed;
                pending.delay = pending.delay > elapsed ? pending.delay - elapsed : 0ms;
                if (pending.delay > 0ms)
                {
                    ++itr;
                    continue;
                }

                due.emplace_back(itr->first, pending);
                itr = pendingActions.erase(itr);
            }
        }

        for (auto& [guid, pending] : due)
        {
            if (ProcessAction(guid, pending))
                continue;

            if (pending.age >= PENDING_TIMEOUT)
            {
                LOG_WARN("module", "mod-hardcore-high-risk: gave up on bot death action {} for {}.",
                    uint32(pending.action), guid.ToString());
                continue;
            }

            std::lock_guard<std::mutex> guard(pendingLock);
            pending.delay = RETRY_DELAY;
            pendingActions.try_emplace(guid, pending);
        }
    }
}

#else

// Without mod-playerbots there are no bot sessions.
namespace HardcoreBotDeath
{
    void OnRandomBotDeath(Player* /*bot*/) { }
    void OnRandomBotLogin(Player* /*bot*/) { }
    void OnAltBotFallen(Player* /*bot*/) { }
    void MaintainAltBotGhost(Player* /*bot*/) { }
    void Update(uint32 /*diff*/) { }
}

#endif
