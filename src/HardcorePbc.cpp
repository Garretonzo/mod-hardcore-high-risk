/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"

#if __has_include("pbc_config.h")
#include "DatabaseEnv.h"
#include "pbc_config.h"
#include "pbc_utils.h"
#include <ctime>
#include <mutex>

namespace
{
    // mod-pbc's scale is 1-10, 10 being life-changing; the highest is always chosen for the prompt.
    constexpr uint8 DEATH_MEMORY_IMPORTANCE = 10;
}

namespace HardcorePbc
{
    // Same steps as mod-pbc's own condensation (pbc_condense.cpp): store the row, then the in-memory copy.
    void AddMemory(ObjectGuid::LowType characterGuid, std::string const& text)
    {
        if (!g_PBC_Enable)
            return;

        std::string escaped = text;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.Execute("INSERT INTO `mod_pbc_memories` (`bot_guid`, `memory_text`, `importance`) "
            "VALUES ({}, '{}', {})", characterGuid, escaped, uint32(DEATH_MEMORY_IMPORTANCE));

        PBC_MemoryEntry entry;
        entry.dbId = 0;
        entry.text = text;
        entry.importance = DEATH_MEMORY_IMPORTANCE;
        entry.createdAt = PBC_FormatDate(std::time(nullptr));

        std::lock_guard<std::mutex> guard(g_PBC_MemoriesMutex);
        g_PBC_Memories[characterGuid].push_back(std::move(entry));
    }
}

#else

namespace HardcorePbc
{
    void AddMemory(ObjectGuid::LowType /*characterGuid*/, std::string const& /*text*/) { }
}

#endif
