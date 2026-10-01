/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"
#include "ChallengeModes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#if HARDCORE_HIGH_RISK_PLAYERBOTS
#include "CalendarMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "Util.h"
#include "World.h"
#include "WorldSessionMgr.h"
#include <mutex>
#include <unordered_map>

namespace
{
    enum class PendingAction : uint8
    {
        AltBotPermaDeath, // altbot just died: tell the master, take it out of the group, log it out
        RefuseLogin,      // a permadead altbot came online by some path: same, with a refusal message
        RandomBotDeath,   // random-account bot died: reset or retire per config
        DeleteCharacter,  // retire, second step: delete once the bot is offline
    };

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

    void TellMaster(Player* bot, std::string const& text)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        Player* master = botAI ? botAI->GetMaster() : nullptr;
        if (master && master != bot && master->GetSession())
            ChatHandler(master->GetSession()).SendSysMessage(text);
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

    bool ProcessAltBotLogout(ObjectGuid guid, PendingAction action)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(guid);
        if (!bot)
            return true;

        if (!FindHolder(bot))
            return false;

        std::string const text = action == PendingAction::RefuseLogin
            ? Acore::StringFormat("{} fell in hardcore and cannot return.", bot->GetName())
            : Acore::StringFormat("{} has fallen and is gone forever.", bot->GetName());
        TellMaster(bot, text);

        return LogoutBot(guid);
    }

    bool ResetRandomBot(Player* bot)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI || !bot->IsInWorld() || bot->IsBeingTeleported())
            return false;

        // Clear the shrine flags first, otherwise the resurrect is blocked (IsPermaDead) or undone by challenge-modes.
        bot->UpdatePlayerSetting(HardcoreHighRisk::CHALLENGE_MODES_SOURCE, SETTING_HARDCORE, 0);
        bot->UpdatePlayerSetting(HardcoreHighRisk::CHALLENGE_MODES_SOURCE, HARDCORE_DEAD, 0);
        HardcoreHighRisk::SetModuleSetting(bot, SETTING_PENDING_RANDOM_BOT_DEATH, 0);

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

        // Same steps as WorldSession::HandleCharDeleteOpcode.
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
            case PendingAction::AltBotPermaDeath:
            case PendingAction::RefuseLogin:
                return ProcessAltBotLogout(guid, pending.action);
            case PendingAction::RandomBotDeath:
                return ProcessRandomBotDeath(guid);
            case PendingAction::DeleteCharacter:
                return DeleteCharacter(guid, pending.accountId);
        }

        return true;
    }
}

namespace HardcoreBotDeath
{
    void OnHardcoreDeath(Player* player)
    {
        if (!HardcoreHighRisk::IsBotSession(player))
            return;

        Milliseconds const delay = Seconds(
            HardcoreHighRisk::GetConfig<uint32>(HardcoreHighRiskConfig::BOT_DEATH_ACTION_DELAY));

        if (HardcoreHighRisk::IsAltBot(player))
        {
            HardcoreHighRisk::SetModuleSetting(player, SETTING_PERMADEAD, 1);
            QueueAction(player->GetGUID(), PendingAction::AltBotPermaDeath, delay);
        }
        else if (HardcoreHighRisk::IsRandomAccountBot(player))
        {
            HardcoreHighRisk::SetModuleSetting(player, SETTING_PENDING_RANDOM_BOT_DEATH, 1);
            QueueAction(player->GetGUID(), PendingAction::RandomBotDeath, delay);
        }
    }

    void OnLogin(Player* player)
    {
        if (!HardcoreHighRisk::IsBotSession(player))
        {
            // A real player on a character this module killed for good, e.g. logging into your own fallen altbot.
            if (player->GetPlayerSetting(HardcoreHighRisk::SETTING_SOURCE, SETTING_PERMADEAD).value)
            {
                if (player->IsAlive())
                    player->KillPlayer();

                player->GetSession()->KickPlayer("Hardcore character has fallen");
            }

            return;
        }

        // The bot is not registered with its holder yet and HandlePlayerBotLoginCallback still uses it after this
        // hook, so it is only flagged here; Update() logs it out once playerbots has finished the login.
        if (HardcoreHighRisk::IsRandomAccountBot(player))
        {
            bool const pending =
                player->GetPlayerSetting(HardcoreHighRisk::SETTING_SOURCE, SETTING_PENDING_RANDOM_BOT_DEATH).value;
            if (pending || HardcoreHighRisk::IsPermaDead(player))
                QueueAction(player->GetGUID(), PendingAction::RandomBotDeath, 0ms);

            return;
        }

        if (HardcoreHighRisk::IsPermaDead(player))
        {
            if (player->IsAlive())
                player->KillPlayer();

            QueueAction(player->GetGUID(), PendingAction::RefuseLogin, 0ms);
        }
    }

    bool CanResurrect(Player* player)
    {
        // challenge-modes' kick cannot remove a bot (no socket), so a permadead bot would revive and be re-killed
        // forever. Real players keep challenge-modes' own handling.
        return !(HardcoreHighRisk::IsBotSession(player) && HardcoreHighRisk::IsPermaDead(player));
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

// Layer one of the re-add guard: refuse fallen altbots before playerbots logs them in, so there is no login,
// no greeting and no party invite. Other paths (autologin, addaccount, mod-pbc) are caught by OnLogin.
class HardcoreBotDeathCommandScript : public AllCommandScript
{
public:
    HardcoreBotDeathCommandScript() : AllCommandScript("HardcoreBotDeathCommandScript", {
        ALLCOMMANDHOOK_ON_TRY_EXECUTE_COMMAND
    }) { }

    bool OnTryExecuteCommand(ChatHandler& handler, std::string_view cmdStr) override
    {
        if (!HardcoreHighRisk::IsEnabled() || !handler.GetSession())
            return true;

        // ".playerbots bot add|login <name[,name...]>"; the command parser accepts unambiguous prefixes.
        std::vector<std::string_view> const tokens = Acore::Tokenize(cmdStr, ' ', false);
        if (tokens.size() != 4 || !StringStartsWithI("playerbots", tokens[0]) || !StringStartsWithI("bot", tokens[1]))
            return true;

        if (tokens[2] != "add" && tokens[2] != "login")
            return true;

        std::vector<std::string> fallen;
        std::string remaining;
        for (std::string_view const name : Acore::Tokenize(tokens[3], ',', false))
        {
            std::string normalized(name);
            if (normalizePlayerName(normalized))
            {
                ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(normalized);
                if (guid && HardcoreHighRisk::IsPermaDead(guid))
                {
                    fallen.push_back(normalized);
                    continue;
                }
            }

            if (!remaining.empty())
                remaining += ',';
            remaining += name;
        }

        if (fallen.empty())
            return true;

        for (std::string const& name : fallen)
            handler.PSendSysMessage("{} fell in hardcore and cannot return.", name);

        if (!remaining.empty())
        {
            std::string args = Acore::StringFormat("{} {}", tokens[2], remaining);
            PlayerbotMgr::HandlePlayerbotMgrCommand(&handler, args.data());
        }

        return false;
    }
};

void AddSC_hardcore_bot_death()
{
    new HardcoreBotDeathCommandScript();
}

#else

// Without mod-playerbots there are no bot sessions; only the real-player login guard applies.
namespace HardcoreBotDeath
{
    void OnHardcoreDeath(Player* /*player*/) { }

    void OnLogin(Player* player)
    {
        if (!player->GetPlayerSetting(HardcoreHighRisk::SETTING_SOURCE, SETTING_PERMADEAD).value)
            return;

        if (player->IsAlive())
            player->KillPlayer();

        player->GetSession()->KickPlayer("Hardcore character has fallen");
    }

    bool CanResurrect(Player* /*player*/)
    {
        return true;
    }

    void Update(uint32 /*diff*/) { }
}

void AddSC_hardcore_bot_death() { }

#endif
