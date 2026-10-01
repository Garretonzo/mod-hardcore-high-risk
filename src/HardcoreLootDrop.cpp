/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HardcoreHighRisk.h"
#include "Bag.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "LootMgr.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include <array>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
    // The 3.3.5a client cannot show more than MAX_NR_LOOT_ITEMS items in one loot window.
    constexpr std::size_t ITEMS_PER_CHEST = MAX_NR_LOOT_ITEMS;
    // LootItem::count is an 8-bit field.
    constexpr uint32 MAX_LOOT_STACK = 255;
    constexpr float CHEST_RADIUS = 2.0f;
    constexpr float CHEST_MIN_SPACING = 1.8f;
    constexpr Seconds REGISTRY_GRACE = 10s;
    constexpr Milliseconds REGISTRY_PRUNE_INTERVAL = 10s;

    struct EnchantmentData
    {
        uint32 id = 0;
        uint32 duration = 0;
        uint32 charges = 0;
    };

    // Per-instance item data that a freshly looted item would otherwise lose.
    struct DroppedItem
    {
        uint32 entry = 0;
        uint32 count = 0;
        int32 randomPropertyId = 0;
        uint32 randomSuffix = 0;
        // Permanent, temporary, socket, bonus and prismatic slots.
        // Random-property slots are rebuilt from randomPropertyId.
        std::array<EnchantmentData, MAX_INSPECTED_ENCHANTMENT_SLOT> enchantments{};
        uint32 durability = 0;
        ObjectGuid creator;
        std::array<int32, MAX_ITEM_PROTO_SPELLS> spellCharges{};
    };

    struct ItemSlot
    {
        uint8 bag;
        uint8 slot;
    };

    struct ChestRecord
    {
        std::vector<DroppedItem> items;
        std::vector<bool> restored;
        Seconds expiry;
    };

    std::mutex chestRegistryLock;
    std::unordered_map<ObjectGuid, ChestRecord> chestRegistry;
    Milliseconds pruneTimer = 0ms;

    DroppedItem CaptureItem(Item const* item)
    {
        DroppedItem data;
        data.entry = item->GetEntry();
        data.randomPropertyId = item->GetItemRandomPropertyId();
        data.randomSuffix = item->GetItemSuffixFactor();

        for (uint8 slot = PERM_ENCHANTMENT_SLOT; slot < MAX_INSPECTED_ENCHANTMENT_SLOT; ++slot)
        {
            EnchantmentSlot const enchantSlot = EnchantmentSlot(slot);
            data.enchantments[slot] = { item->GetEnchantmentId(enchantSlot), item->GetEnchantmentDuration(enchantSlot),
                item->GetEnchantmentCharges(enchantSlot) };
        }

        data.durability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        data.creator = item->GetGuidValue(ITEM_FIELD_CREATOR);

        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            data.spellCharges[i] = item->GetSpellCharges(i);

        return data;
    }

    // Gift-wrapped items keep their contents in character_gifts under the original item guid; a looted copy would be
    // an empty wrapper, so they stay on the character.
    bool CanDrop(Item const* item)
    {
        return item && item->GetTemplate() && !item->IsWrapped();
    }

    void AppendItem(Item const* item, std::vector<DroppedItem>& out)
    {
        DroppedItem const data = CaptureItem(item);
        uint32 remaining = item->GetCount();
        do
        {
            DroppedItem part = data;
            part.count = std::min(remaining, MAX_LOOT_STACK);
            out.push_back(part);
            remaining -= part.count;
        } while (remaining > 0);
    }

    void CollectSlot(Player* player, uint8 bag, uint8 slot, std::vector<DroppedItem>& out,
        std::vector<ItemSlot>& destroy)
    {
        Item* item = player->GetItemByPos(bag, slot);
        if (!CanDrop(item))
            return;

        AppendItem(item, out);
        destroy.push_back({ bag, slot });
    }

    void CollectItems(Player* player, std::vector<DroppedItem>& gear, std::vector<DroppedItem>& inventory,
        std::vector<ItemSlot>& destroy)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            CollectSlot(player, INVENTORY_SLOT_BAG_0, slot, gear, destroy);

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            CollectSlot(player, INVENTORY_SLOT_BAG_0, slot, inventory, destroy);

        for (uint8 slot = KEYRING_SLOT_START; slot < KEYRING_SLOT_END; ++slot)
            CollectSlot(player, INVENTORY_SLOT_BAG_0, slot, inventory, destroy);

        // Bag contents first, so they are destroyed before the bag that holds them.
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = player->GetBagByPos(bagSlot);
            if (!bag)
                continue;

            bool keepsItems = false;
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            {
                Item* item = bag->GetItemByPos(uint8(slot));
                if (!item)
                    continue;

                if (!CanDrop(item))
                {
                    keepsItems = true;
                    continue;
                }

                AppendItem(item, inventory);
                destroy.push_back({ bagSlot, uint8(slot) });
            }

            // A bag still holding a kept item stays equipped, otherwise destroying it would destroy that item too.
            if (!keepsItems)
                CollectSlot(player, INVENTORY_SLOT_BAG_0, bagSlot, inventory, destroy);
        }
    }

    void FillChest(GameObject* chest, std::vector<DroppedItem> const& items, uint32 gold)
    {
        Loot& loot = chest->loot;
        loot.clear();
        loot.items.reserve(items.size());

        for (std::size_t i = 0; i < items.size(); ++i)
        {
            DroppedItem const& data = items[i];

            LootItem lootItem;
            lootItem.itemid = data.entry;
            lootItem.itemIndex = uint32(i);
            lootItem.randomSuffix = data.randomSuffix;
            lootItem.randomPropertyId = data.randomPropertyId;
            lootItem.count = uint8(data.count);
            lootItem.is_looted = false;
            lootItem.is_blocked = false;
            lootItem.freeforall = false;
            lootItem.is_underthreshold = true;
            lootItem.is_counted = false;
            lootItem.needs_quest = false;
            lootItem.follow_loot_rules = false;
            lootItem.groupid = 0;
            loot.items.push_back(lootItem);
        }

        loot.unlootedCount = uint8(items.size());
        loot.gold = gold;
        loot.sourceWorldObjectGUID = chest->GetGUID();
    }

    GameObject* SpawnChest(Player* player, uint32 entry, float angle, float radius, uint32 duration)
    {
        float x, y, z;
        player->GetNearPoint(player, x, y, z, 0.0f, radius, angle);

        // Face the body.
        float const orientation = Position::NormalizeOrientation(angle + float(M_PI));
        GameObject* chest = player->GetMap()->SummonGameObject(entry, x, y, z, orientation, 0.0f, 0.0f,
            std::sin(orientation / 2.0f), std::cos(orientation / 2.0f), duration);
        if (!chest)
            return nullptr;

        chest->SetPhaseMask(player->GetPhaseMask(), true);
        // The summon respawn timer only runs while the chest is untouched; this one also covers an opened chest.
        chest->DespawnOrUnsummon(Seconds(duration));
        return chest;
    }

    // Pointer comparison only: another OnPlayerLootItem script (e.g. mod-junk-to-gold) may already have destroyed and
    // freed the new item, so it must not be dereferenced until it is found in the inventory.
    bool IsInInventory(Player* player, Item const* item)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) == item)
                return true;

        for (uint8 slot = KEYRING_SLOT_START; slot < CURRENCYTOKEN_SLOT_END; ++slot)
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) == item)
                return true;

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (bag->GetItemByPos(uint8(slot)) == item)
                        return true;

        return false;
    }

    void RestoreItem(Player* player, Item* item, DroppedItem const& data)
    {
        for (uint8 slot = PERM_ENCHANTMENT_SLOT; slot < MAX_INSPECTED_ENCHANTMENT_SLOT; ++slot)
        {
            EnchantmentData const& enchantment = data.enchantments[slot];
            if (enchantment.id)
                item->SetEnchantment(EnchantmentSlot(slot), enchantment.id, enchantment.duration, enchantment.charges);
        }

        if (uint32 maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
            item->SetUInt32Value(ITEM_FIELD_DURABILITY, std::min(data.durability, maxDurability));

        if (data.creator)
            item->SetGuidValue(ITEM_FIELD_CREATOR, data.creator);

        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            item->SetSpellCharges(i, data.spellCharges[i]);

        item->SetState(ITEM_CHANGED, player);
    }
}

namespace HardcoreLootDrop
{
    void OnHardcoreDeath(Player* player)
    {
        using HardcoreHighRisk::GetConfig;

        if (!GetConfig<bool>(HardcoreHighRiskConfig::LOOT_DROP_ENABLED))
            return;

        if (!player->IsInWorld() || !player->FindMap())
            return;

        std::vector<DroppedItem> gear;
        std::vector<DroppedItem> inventory;
        std::vector<ItemSlot> destroy;
        CollectItems(player, gear, inventory, destroy);

        uint32 const gold = GetConfig<bool>(HardcoreHighRiskConfig::LOOT_DROP_GOLD) ? player->GetMoney() : 0;

        // Repeat deaths (login re-kill, Spirit of Redemption) find nothing left and stop here.
        if (gear.empty() && inventory.empty() && !gold)
            return;

        // One entry per chest: template, its items, its gold.
        struct ChestPlan
        {
            uint32 entry;
            std::vector<DroppedItem> items;
            uint32 gold;
        };
        std::vector<ChestPlan> plans;

        auto planChests = [&plans](uint32 entry, std::vector<DroppedItem> const& items, uint32 firstChestGold)
        {
            // A gear chest is still needed for the gold when no gear is left.
            bool const goldOnly = items.empty() && firstChestGold;
            for (std::size_t first = 0; first < items.size() || (first == 0 && goldOnly); first += ITEMS_PER_CHEST)
            {
                auto const begin = items.begin() + std::ptrdiff_t(first);
                auto const end = items.begin() + std::ptrdiff_t(std::min(items.size(), first + ITEMS_PER_CHEST));
                std::vector<DroppedItem> chunk(begin, end);
                plans.push_back({ entry, std::move(chunk), first == 0 ? firstChestGold : 0 });
            }
        };

        planChests(GetConfig<uint32>(HardcoreHighRiskConfig::LOOT_DROP_GEAR_CHEST_ENTRY), gear, gold);
        planChests(GetConfig<uint32>(HardcoreHighRiskConfig::LOOT_DROP_INVENTORY_CHEST_ENTRY), inventory, 0);

        // Two chests sit either side of the body; more form an evenly spaced ring wide enough to click each one.
        std::size_t const chestCount = plans.size();
        float const radius = std::max(CHEST_RADIUS, float(chestCount) * CHEST_MIN_SPACING / (2.0f * float(M_PI)));
        float const firstAngle = player->GetOrientation() + float(M_PI) / 2.0f;
        uint32 const duration = GetConfig<uint32>(HardcoreHighRiskConfig::LOOT_DROP_CHEST_DURATION);

        std::vector<GameObject*> chests;
        chests.reserve(chestCount);
        for (std::size_t i = 0; i < chestCount; ++i)
        {
            float const angle = firstAngle + float(i) * 2.0f * float(M_PI) / float(chestCount);
            GameObject* chest = SpawnChest(player, plans[i].entry, angle, radius, duration);
            if (!chest)
            {
                // Nothing is taken from the character unless every chest exists.
                LOG_ERROR("module", "mod-hardcore-high-risk: could not spawn chest (entry {}) for {}; nothing dropped.",
                    plans[i].entry, player->GetGUID().ToString());
                for (GameObject* spawned : chests)
                    spawned->DespawnOrUnsummon();
                return;
            }

            chests.push_back(chest);
        }

        Seconds const expiry = GameTime::GetGameTime() + Seconds(duration) + REGISTRY_GRACE;
        {
            std::lock_guard<std::mutex> guard(chestRegistryLock);
            for (std::size_t i = 0; i < chestCount; ++i)
            {
                FillChest(chests[i], plans[i].items, plans[i].gold);
                std::vector<bool> restored(plans[i].items.size(), false);
                chestRegistry[chests[i]->GetGUID()] = { std::move(plans[i].items), std::move(restored), expiry };
            }
        }

        for (ItemSlot const& itemSlot : destroy)
            player->DestroyItem(itemSlot.bag, itemSlot.slot, true);

        if (gold)
            player->SetMoney(0);

        // Save immediately so a crash cannot leave the items both in the chests and on the character.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        if (!HardcoreHighRisk::IsBotSession(player))
            ChatHandler(player->GetSession()).SendSysMessage("Everything you carried spills onto the ground.");
    }

    void Update(uint32 diff)
    {
        pruneTimer += Milliseconds(diff);
        if (pruneTimer < REGISTRY_PRUNE_INTERVAL)
            return;

        pruneTimer = 0ms;
        Seconds const now = GameTime::GetGameTime();

        std::lock_guard<std::mutex> guard(chestRegistryLock);
        for (auto itr = chestRegistry.begin(); itr != chestRegistry.end();)
        {
            if (itr->second.expiry <= now)
                itr = chestRegistry.erase(itr);
            else
                ++itr;
        }
    }
}

class HardcoreLootDropPlayerScript : public PlayerScript
{
public:
    HardcoreLootDropPlayerScript() : PlayerScript("HardcoreLootDropPlayerScript", {
        PLAYERHOOK_ON_LOOT_ITEM
    }) { }

    void OnPlayerLootItem(Player* player, Item* item, uint32 /*count*/, ObjectGuid lootguid) override
    {
        if (!item || !lootguid.IsGameObject())
            return;

        {
            std::lock_guard<std::mutex> guard(chestRegistryLock);
            if (!chestRegistry.contains(lootguid))
                return;
        }

        GameObject* chest = player->GetMap()->GetGameObject(lootguid);
        if (!chest || !IsInInventory(player, item))
            return;

        uint32 const entry = item->GetEntry();
        DroppedItem data;
        {
            std::lock_guard<std::mutex> guard(chestRegistryLock);
            auto itr = chestRegistry.find(lootguid);
            if (itr == chestRegistry.end())
                return;

            // StoreLootItem flags the slot as looted before this hook runs, so this item is the looted slot of the
            // same entry that has not been restored yet.
            ChestRecord& record = itr->second;
            std::vector<LootItem> const& lootItems = chest->loot.items;
            std::size_t const slotCount = std::min(record.items.size(), lootItems.size());
            std::size_t index = 0;
            for (; index < slotCount; ++index)
                if (lootItems[index].is_looted && !record.restored[index] && record.items[index].entry == entry)
                    break;

            if (index >= slotCount)
                return;

            record.restored[index] = true;
            data = record.items[index];
        }

        RestoreItem(player, item, data);
    }
};

void AddSC_hardcore_loot_drop()
{
    new HardcoreLootDropPlayerScript();
}
