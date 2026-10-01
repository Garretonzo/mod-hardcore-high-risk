/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Group.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr float COMPANION_RANGE = 40.0f;
    // A captured story older than this belongs to some other death and is ignored.
    constexpr Seconds STORY_MAX_AGE = 60s;

    struct CapturedStory
    {
        HardcoreDeathContext::DeathStory story;
        Seconds capturedAt;
    };

    std::mutex storyLock;
    std::unordered_map<ObjectGuid::LowType, CapturedStory> capturedStories;

    char const* RaceName(uint8 race)
    {
        switch (race)
        {
            case RACE_HUMAN:         return "human";
            case RACE_ORC:           return "orc";
            case RACE_DWARF:         return "dwarf";
            case RACE_NIGHTELF:      return "night elf";
            case RACE_UNDEAD_PLAYER: return "undead";
            case RACE_TAUREN:        return "tauren";
            case RACE_GNOME:         return "gnome";
            case RACE_TROLL:         return "troll";
            case RACE_BLOODELF:      return "blood elf";
            case RACE_DRAENEI:       return "draenei";
            default:                 return "";
        }
    }

    char const* ClassName(uint8 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:      return "warrior";
            case CLASS_PALADIN:      return "paladin";
            case CLASS_HUNTER:       return "hunter";
            case CLASS_ROGUE:        return "rogue";
            case CLASS_PRIEST:       return "priest";
            case CLASS_DEATH_KNIGHT: return "death knight";
            case CLASS_SHAMAN:       return "shaman";
            case CLASS_MAGE:         return "mage";
            case CLASS_WARLOCK:      return "warlock";
            case CLASS_DRUID:        return "druid";
            default:                 return "";
        }
    }

    std::string WithArticle(std::string const& phrase)
    {
        if (phrase.empty())
            return phrase;

        char const first = char(std::tolower(static_cast<unsigned char>(phrase.front())));
        bool const vowel = first == 'a' || first == 'e' || first == 'i' || first == 'o' || first == 'u';
        return (vowel ? "an " : "a ") + phrase;
    }

    // "A", "A and B", "A, B and C".
    std::string JoinNames(std::vector<std::string> const& names)
    {
        std::string joined;
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (i > 0)
                joined += (i + 1 == names.size()) ? " and " : ", ";
            joined += names[i];
        }
        return joined;
    }

    std::string AreaName(uint32 areaId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
        return area ? area->area_name[sWorld->GetDefaultDbcLocale()] : "";
    }

    std::string DescribePlace(Player* victim)
    {
        if (Map* map = victim->GetMap())
            if (map->Instanceable())
                return map->GetMapName();

        std::string const zone = AreaName(victim->GetZoneId());
        std::string const area = AreaName(victim->GetAreaId());
        if (!area.empty() && area != zone)
            return zone.empty() ? area : area + ", " + zone;

        return zone.empty() ? "the wilds" : zone;
    }

    std::string DescribeCreature(Creature const* creature)
    {
        std::string rank;
        switch (creature->GetCreatureTemplate()->rank)
        {
            case CREATURE_ELITE_ELITE:     rank = "elite "; break;
            case CREATURE_ELITE_RAREELITE: rank = "rare elite "; break;
            case CREATURE_ELITE_RARE:      rank = "rare "; break;
            case CREATURE_ELITE_WORLDBOSS: return "the mighty " + creature->GetName();
            default: break;
        }

        return WithArticle(Acore::StringFormat("level {} {}{}", creature->GetLevel(), rank, creature->GetName()));
    }

    std::string DescribePlayer(Player const* player)
    {
        return Acore::StringFormat("{}, a level {} {} {}", player->GetName(), player->GetLevel(),
            RaceName(player->getRace()), ClassName(player->getClass()));
    }

    // The unit the character was fighting, as a short name ("a Kobold Tunneler", "Bob").
    std::string DescribeOpponent(Unit const* unit)
    {
        if (Creature const* creature = unit->ToCreature())
            return WithArticle(creature->GetName());

        return unit->GetName();
    }

    // Active quest with a kill objective for one of these creatures.
    std::string FindQuestFor(Player* victim, std::vector<uint32> const& creatureEntries)
    {
        for (auto const& [questId, status] : victim->getQuestStatusMap())
        {
            if (status.Status != QUEST_STATUS_INCOMPLETE)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const required = quest->RequiredNpcOrGo[i];
                for (uint32 entry : creatureEntries)
                    if (required > 0 && uint32(required) == entry)
                        return quest->GetTitle();
            }
        }

        return "";
    }

    HardcoreDeathContext::DeathStory BuildStory(Player* victim, Unit* killer, bool killingBlowSeen)
    {
        HardcoreDeathContext::DeathStory story;
        story.name = victim->GetName();
        story.date = Acore::Time::TimeToTimestampStr(GameTime::GetGameTime(), "%Y-%m-%d");
        story.place = DescribePlace(victim);
        story.level = victim->GetLevel();

        if (!killingBlowSeen)
            return story;

        // Pets, totems and guardians count as their owner.
        Player* killerPlayer = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (killerPlayer && killerPlayer != victim)
            story.cause = "slain by " + DescribePlayer(killerPlayer);
        else if (killer && killer != victim && killer->ToCreature())
            story.cause = "slain by " + DescribeCreature(killer->ToCreature());
        else if (victim->IsUnderWater())
            story.cause = "by drowning";
        else if (victim->IsFalling())
            story.cause = "from a fatal fall";
        else
            story.cause = "to the hazards of the world";

        std::vector<uint32> creatureEntries;
        if (killer && killer->ToCreature())
            creatureEntries.push_back(killer->GetEntry());

        Unit* opponent = victim->GetVictim();
        std::string fighting;
        if (opponent && opponent != killer && opponent != victim)
        {
            fighting = DescribeOpponent(opponent);
            if (opponent->ToCreature())
                creatureEntries.push_back(opponent->GetEntry());
        }

        std::string const quest = FindQuestFor(victim, creatureEntries);
        if (!fighting.empty())
            story.doing += " while fighting " + fighting;
        if (!quest.empty())
            story.doing += (fighting.empty() ? " while on the quest \"" : " for the quest \"") + quest + "\"";

        std::size_t const attackers = victim->getAttackers().size();
        if (attackers >= 2)
            story.doing += Acore::StringFormat(", beset by {} enemies", attackers);

        if (Group* group = victim->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && member != victim && member->IsInMap(victim) && member->IsAlive()
                    && victim->IsWithinDistInMap(member, COMPANION_RANGE))
                    story.present.push_back(member->GetName());
            }
        }

        return story;
    }
}

namespace HardcoreDeathContext
{
    std::string DeathStory::ForSelf() const
    {
        std::string text = Acore::StringFormat("You died on {} in {} at level {}", date, place, level);
        if (!cause.empty())
            text += ", " + cause + doing;
        text += present.empty() ? ", alone" : ", with " + JoinNames(present) + " at your side";
        text += ". You were hardcore, so this death is permanent: you are now a ghost who can never be resurrected, "
            "and everything you carried was left on the ground.";
        return text;
    }

    std::string DeathStory::ForCompanion(std::string const& companionName) const
    {
        std::vector<std::string> witnesses = present;
        for (std::string& witness : witnesses)
            if (witness == companionName)
                witness = "you";

        std::string text = Acore::StringFormat("{} died for good on {} in {} at level {}", name, date, place, level);
        if (!cause.empty())
            text += ", " + cause + doing;
        if (!witnesses.empty())
            text += ", with " + JoinNames(witnesses) + " at their side";
        text += Acore::StringFormat(". {} was hardcore and is now a ghost who can never be resurrected.", name);
        return text;
    }

    void Capture(Player* victim, Unit* killer)
    {
        CapturedStory captured{ BuildStory(victim, killer, true), GameTime::GetGameTime() };
        std::lock_guard<std::mutex> guard(storyLock);
        capturedStories[victim->GetGUID().GetCounter()] = std::move(captured);
    }

    DeathStory Take(Player* victim)
    {
        {
            std::lock_guard<std::mutex> guard(storyLock);
            auto itr = capturedStories.find(victim->GetGUID().GetCounter());
            if (itr != capturedStories.end())
            {
                CapturedStory captured = std::move(itr->second);
                capturedStories.erase(itr);
                if (GameTime::GetGameTime() - captured.capturedAt <= STORY_MAX_AGE)
                    return captured.story;
            }
        }

        // Died without a killing blow passing through Unit::Kill: only where and when are known.
        return BuildStory(victim, nullptr, false);
    }
}

class HardcoreDeathContextUnitScript : public UnitScript
{
public:
    HardcoreDeathContextUnitScript() : UnitScript("HardcoreDeathContextUnitScript", true, {
        UNITHOOK_ON_UNIT_DEATH
    }) { }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        Player* victim = unit ? unit->ToPlayer() : nullptr;
        if (!victim || !HardcoreHighRisk::IsEnabled() || !HardcoreHighRisk::IsHardcore(victim))
            return;

        // Random bots are recycled, and the fallen are not fallen twice.
        if (HardcoreHighRisk::IsRandomAccountBot(victim) || HardcoreHighRisk::IsPermaDead(victim))
            return;

        HardcoreDeathContext::Capture(victim, killer);
    }
};

void AddSC_hardcore_death_context()
{
    new HardcoreDeathContextUnitScript();
}
