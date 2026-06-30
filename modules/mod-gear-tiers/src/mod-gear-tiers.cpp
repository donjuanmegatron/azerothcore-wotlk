// mod-gear-tiers.cpp
//
// Sanctum Armory Slot System
// --------------------------
// The client addon (SanctumArmorySlot) shows a custom slot next to the ranged
// slot on the character frame.  When the player drops any item on it, the addon
// sends a chat command to this module:
//
//   .armoryslot set entry <itemEntryId>   -- designates that item
//   .armoryslot clear                     -- removes designation
//   .armoryslot status                    -- shows current item + GXP
//
// Designated items must be Uncommon (green) quality or higher.
//
// Tier Progression (item physically morphs on tier-up):
//   Normal (green original)  →  10,000 GXP  →  Enchanted (blue/Rare)
//   Enchanted (blue)         →  60,000 GXP  →  Epic (purple)
//   Legendary (orange) quality is reserved for future class legendary items.
//
// On tier-up the item entry changes: a new item_template row is created with
// a prefixed name, higher quality, and multiplied stats. The old item is
// destroyed and the new one placed in the same slot or bag.
//
// GXP per kill (con-based — grey mobs award 0):
//   Green mob = 1   Yellow mob = 2   Red mob = 3   Elite/Rare = 5   Raid = 1 (cap)
//
// All base items receive bonus Stamina and Intellect scaled by item level.
// Armory item is exempt from death durability loss.
// Item binds to player on first GXP earned.
// Server pushes SANCTUMARMORY: sync messages to client after every kill.
//
// Loot Tier Rolling:
//   When a player loots/receives gear it gets a random Sanctum tier applied:
//   Uncommon: 70% Normal / 25% Enchanted / 5% Epic
//   Rare:     60% Normal / 30% Enchanted / 10% Epic
//   Epic:     0% Normal  / 20% Enchanted / 80% Epic
//   If variants don't exist yet (first time seeing this item), they are queued
//   for creation and the item stays Normal until the next server restart.
//   Use ".gear roll" to retroactively roll all items in inventory.

#include "ScriptMgr.h"
#include "Player.h"
#include "Item.h"
#include "Bag.h"
#include "Creature.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "CommandScript.h"
#include "ObjectMgr.h"
#include "Log.h"
#include "Random.h"
#include "DBCStores.h"
#include "QuestDef.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <set>

using namespace Acore::ChatCommands;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint32 GXP_GREEN_KILL  = 1;
static constexpr uint32 GXP_YELLOW_KILL = 2;
static constexpr uint32 GXP_RED_KILL    = 3;
static constexpr uint32 GXP_ELITE_KILL  = 5;
static constexpr uint32 GXP_RAID_KILL   = 1;

static constexpr uint32 GXP_TIER1_THRESHOLD = 10000;
static constexpr uint32 GXP_TIER2_THRESHOLD = 60000;

static constexpr uint8 TIER_NORMAL    = 0;
static constexpr uint8 TIER_ENCHANTED = 1;
static constexpr uint8 TIER_EPIC      = 2;   // Purple. Orange (Legendary) reserved for class legendaries.

// WoW item stat type IDs for the HP/Mana bonus added to all variants
static constexpr uint8 ITEM_STAT_STAMINA   = 7;
static constexpr uint8 ITEM_STAT_INTELLECT = 5;

// All Sanctum-generated variant entries live in this range
static constexpr uint32 SANCTUM_ENTRY_MIN = 9000000;

// Minimum item quality allowed for Armory Slot designation (2 = Uncommon/green)
static constexpr uint8 ARMORY_MIN_QUALITY = 2;

// ---------------------------------------------------------------------------
// Per-character runtime data
// ---------------------------------------------------------------------------

struct ArmoryData
{
    uint32 itemEntry = 0;   // Current entry (may be a variant entry after morphing)
    uint32 gearXP    = 0;   // Total accumulated Gear XP
    uint8  tier      = 0;   // 0=Normal 1=Enchanted 2=Epic
    bool   dirty     = false;
};

static std::unordered_map<uint32, ArmoryData> g_armory;

// Enchanted + Epic entry IDs for a base item
struct ItemVariants
{
    uint32 enchantedEntry = 0;
    uint32 epicEntry      = 0;
    bool   valid          = false;
};

// In-memory cache of base_entry → variant entries, loaded at startup.
// TryApplyLootTier checks this cache only — never hits the DB during gameplay.
static std::unordered_map<uint32, ItemVariants> g_variantCache;

// ---------------------------------------------------------------------------
// Core helpers
// ---------------------------------------------------------------------------

static void SaveArmoryData(uint32 playerGuid, ArmoryData& data)
{
    CharacterDatabase.Execute(
        "REPLACE INTO character_armory_slot (guid, item_guid, item_entry, gear_xp, tier) "
        "VALUES ({}, 0, {}, {}, {})",
        playerGuid, data.itemEntry, data.gearXP, data.tier);
    data.dirty = false;
}

static void SendArmorySync(Player* player, const ArmoryData& data)
{
    uint32 nextThreshold = 0;
    if (data.tier == TIER_NORMAL)
        nextThreshold = GXP_TIER1_THRESHOLD;
    else if (data.tier == TIER_ENCHANTED)
        nextThreshold = GXP_TIER2_THRESHOLD;

    ChatHandler(player->GetSession()).PSendSysMessage(
        "SANCTUMARMORY:{}:{}:{}", data.gearXP, (uint32)data.tier, nextThreshold);
}

static Item* FindArmoryItem(Player* player, const ArmoryData& data)
{
    if (!data.itemEntry)
        return nullptr;
    return player->GetItemByEntry(data.itemEntry);
}

static void RepairArmoryItem(Player* player)
{
    uint32 guid = player->GetGUID().GetCounter();
    auto it = g_armory.find(guid);
    if (it == g_armory.end() || !it->second.itemEntry)
        return;

    Item* item = FindArmoryItem(player, it->second);
    if (!item)
        return;

    uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
    if (maxDur == 0)
        return;

    item->SetUInt32Value(ITEM_FIELD_DURABILITY, maxDur);
    item->SetState(ITEM_CHANGED, player);
}

static uint8 GetGrayLevel(uint8 playerLevel)
{
    if (playerLevel <= 5)  return 0;
    if (playerLevel <= 39) return playerLevel - 5 - playerLevel / 10;
    if (playerLevel <= 59) return playerLevel - 1 - playerLevel / 5;
    return playerLevel - (playerLevel - 30) / 3;
}

static const char* TierName(uint8 tier)
{
    switch (tier)
    {
        case TIER_ENCHANTED: return "Enchanted";
        case TIER_EPIC:      return "Epic";
        default:             return "Normal";
    }
}

// ---------------------------------------------------------------------------
// Variant creation
// ---------------------------------------------------------------------------

// Creates a new item_template row derived from baseEntry with:
//   - New entry ID, name prefix, higher quality
//   - All stats/armor/damage multiplied by the tier factor
//   - Bonus Stamina and Intellect added (scaled by item level × multiplier)
//   - BuyPrice/SellPrice zeroed, bonding set to bind-on-pickup
//
// All five SQL statements run inside a single DirectCommitTransaction so
// the TEMPORARY TABLE persists across statements on the same connection.
static void CreateVariantInDB(uint32 baseEntry, uint32 newEntry,
                               const std::string& prefix, uint8 newQuality,
                               float multiplier)
{
    QueryResult r = WorldDatabase.Query(
        "SELECT ItemLevel, "
        "stat_type1,  stat_value1,  stat_type2,  stat_value2, "
        "stat_type3,  stat_value3,  stat_type4,  stat_value4, "
        "stat_type5,  stat_value5,  stat_type6,  stat_value6, "
        "stat_type7,  stat_value7,  stat_type8,  stat_value8, "
        "stat_type9,  stat_value9,  stat_type10, stat_value10 "
        "FROM item_template WHERE entry = {}", baseEntry);

    if (!r)
    {
        LOG_ERROR("module", "[mod-gear-tiers] CreateVariantInDB: base entry {} not in item_template", baseEntry);
        return;
    }

    Field* f   = r->Fetch();
    uint32 ilvl = f[0].Get<uint32>();

    uint8 statTypes[10];
    int32 statValues[10];
    for (int i = 0; i < 10; i++)
    {
        statTypes[i]  = f[1 + i * 2].Get<uint8>();
        statValues[i] = f[2 + i * 2].Get<int32>();
    }

    // Compute statsCount from non-zero type slots (same as ObjectMgr::LoadItemTemplates)
    uint8 statsCount = 0;
    for (int i = 0; i < 10; i++)
        if (statTypes[i] != 0) statsCount = static_cast<uint8>(i + 1);

    // Apply tier multiplier to all existing stats
    for (int i = 0; i < 10; i++)
        if (statTypes[i] != 0)
            statValues[i] = static_cast<int32>(std::round(statValues[i] * multiplier));

    // Stamina/Intellect bonus scaled by both item level and tier multiplier
    uint32 bonusStam = static_cast<uint32>(std::round(std::max(5u, ilvl / 3) * multiplier));
    uint32 bonusInt  = static_cast<uint32>(std::round(std::max(3u, ilvl / 5) * multiplier));

    // Add Stamina — prefer existing slot, otherwise first empty slot
    bool addedStam = false;
    for (int i = 0; i < 10 && !addedStam; i++)
        if (statTypes[i] == ITEM_STAT_STAMINA)
        {
            statValues[i] += static_cast<int32>(bonusStam);
            addedStam = true;
        }
    if (!addedStam)
        for (int i = 0; i < 10; i++)
            if (statTypes[i] == 0)
            {
                statTypes[i]  = ITEM_STAT_STAMINA;
                statValues[i] = static_cast<int32>(bonusStam);
                if (static_cast<uint8>(i + 1) > statsCount) statsCount = static_cast<uint8>(i + 1);
                break;
            }

    // Add Intellect
    bool addedInt = false;
    for (int i = 0; i < 10 && !addedInt; i++)
        if (statTypes[i] == ITEM_STAT_INTELLECT)
        {
            statValues[i] += static_cast<int32>(bonusInt);
            addedInt = true;
        }
    if (!addedInt)
        for (int i = 0; i < 10; i++)
            if (statTypes[i] == 0)
            {
                statTypes[i]  = ITEM_STAT_INTELLECT;
                statValues[i] = static_cast<int32>(bonusInt);
                if (static_cast<uint8>(i + 1) > statsCount) statsCount = static_cast<uint8>(i + 1);
                break;
            }

    // Build the stat portion of the UPDATE
    // Note: StatsCount is not a DB column in this AzerothCore build — computed in C++ from stat_type slots
    std::ostringstream statSql;
    bool first = true;
    for (int i = 0; i < 10; i++)
    {
        if (!first) statSql << ", ";
        statSql << "stat_type"  << (i + 1) << " = " << static_cast<uint32>(statTypes[i])
                << ", stat_value" << (i + 1) << " = " << statValues[i];
        first = false;
    }

    // Unique temp table name per entry avoids any concurrent collisions
    std::string tmp = "tmp_sv_" + std::to_string(newEntry);

    std::ostringstream drop1, create, update, insert, drop2;
    drop1  << "DROP TEMPORARY TABLE IF EXISTS " << tmp;
    create << "CREATE TEMPORARY TABLE " << tmp
           << " SELECT * FROM item_template WHERE entry = " << baseEntry;
    update << "UPDATE " << tmp << " SET "
           << "entry = "    << newEntry    << ", "
           << "name = CONCAT('" << prefix << "', name), "
           << "Quality = "  << static_cast<uint32>(newQuality) << ", "
           << statSql.str() << ", "
           << "armor    = ROUND(armor    * " << multiplier << "), "
           << "dmg_min1 = ROUND(dmg_min1 * " << multiplier << "), "
           << "dmg_max1 = ROUND(dmg_max1 * " << multiplier << "), "
           << "dmg_min2 = ROUND(dmg_min2 * " << multiplier << "), "
           << "dmg_max2 = ROUND(dmg_max2 * " << multiplier << "), "
           << "BuyPrice = 0, SellPrice = 0, bonding = 1";
    insert << "INSERT INTO item_template SELECT * FROM " << tmp;
    drop2  << "DROP TEMPORARY TABLE IF EXISTS " << tmp;

    // Single transaction = single connection = temp table persists across all five statements
    WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
    trans->Append(drop1.str());
    trans->Append(create.str());
    trans->Append(update.str());
    trans->Append(insert.str());
    trans->Append(drop2.str());
    WorldDatabase.DirectCommitTransaction(trans);

    LOG_INFO("module", "[mod-gear-tiers] Created variant entry {} ('{}…', x{:.1f}, quality {})",
        newEntry, prefix, multiplier, static_cast<uint32>(newQuality));
}

// Returns variant entries for baseEntry, creating them if this is the first time.
// On creation, reloads ObjectMgr item templates (~1 sec) so new entries are usable immediately.
// Only called from HandleSetEntry (designation time), not during kills.
static ItemVariants GetOrCreateVariants(uint32 baseEntry)
{
    QueryResult existing = WorldDatabase.Query(
        "SELECT enchanted_entry, epic_entry FROM sanctum_item_variants WHERE base_entry = {}",
        baseEntry);
    if (existing)
    {
        Field* f = existing->Fetch();
        return { f[0].Get<uint32>(), f[1].Get<uint32>(), true };
    }

    if (!sObjectMgr->GetItemTemplate(baseEntry))
    {
        LOG_ERROR("module", "[mod-gear-tiers] GetOrCreateVariants: base entry {} not in ObjectMgr", baseEntry);
        return {};
    }

    // Allocate two consecutive entry IDs in the Sanctum range
    QueryResult maxRes = WorldDatabase.Query(
        "SELECT COALESCE(MAX(entry), {}) FROM item_template WHERE entry >= {}",
        SANCTUM_ENTRY_MIN - 1, SANCTUM_ENTRY_MIN);
    uint32 base      = maxRes ? (*maxRes)[0].Get<uint32>() + 1 : SANCTUM_ENTRY_MIN;
    uint32 enchEntry = base;
    uint32 epicEntry = base + 1;

    CreateVariantInDB(baseEntry, enchEntry, "Enchanted ", 3, 1.4f);
    CreateVariantInDB(baseEntry, epicEntry, "Epic ",      4, 2.0f);

    WorldDatabase.DirectExecute(
        "INSERT INTO sanctum_item_variants (base_entry, enchanted_entry, epic_entry) "
        "VALUES ({}, {}, {})", baseEntry, enchEntry, epicEntry);

    // NOTE: LoadItemTemplates() is NOT called here — calling it mid-session races
    // with active player threads and crashes the server.  Instead, variants are loaded
    // into ObjectMgr by GearTiersWorldScript::OnStartup() at boot time.  If this is
    // a brand-new designation in the current session, the player must relog once for
    // tier upgrades to activate (the variants will be in ObjectMgr on next startup).
    LOG_INFO("module", "[mod-gear-tiers] Created variants {} and {} for base {} — relog required for tier-up.",
        enchEntry, epicEntry, baseEntry);

    return { enchEntry, epicEntry, true };
}

// Read-only variant lookup — used during tier-up so we never trigger a
// LoadItemTemplates() mid-kill. Variants must already exist (created at designation).
static ItemVariants FindVariants(uint32 baseEntry)
{
    QueryResult r = WorldDatabase.Query(
        "SELECT enchanted_entry, epic_entry FROM sanctum_item_variants WHERE base_entry = {}",
        baseEntry);
    if (r)
    {
        Field* f = r->Fetch();
        return { f[0].Get<uint32>(), f[1].Get<uint32>(), true };
    }
    return {};
}

// ---------------------------------------------------------------------------
// Tier upgrade — morphs the physical item
// ---------------------------------------------------------------------------

static void CheckTierUpgrade(Player* player, ArmoryData& data, Item* item)
{
    if (data.tier >= TIER_EPIC)
        return;

    uint32 threshold = (data.tier == TIER_NORMAL) ? GXP_TIER1_THRESHOLD : GXP_TIER2_THRESHOLD;
    if (data.gearXP < threshold)
        return;

    data.tier++;
    data.dirty = true;

    const char* tierHex  = (data.tier == TIER_ENCHANTED) ? "0070dd" : "a335ee";
    const char* tierName = TierName(data.tier);

    // Capture item info now — pointer becomes dangling after DestroyItem
    std::string itemName = item ? item->GetTemplate()->Name1 : "(unknown)";
    uint8 bagSlot        = item ? item->GetBagSlot() : NULL_BAG;
    uint8 itemSlot       = item ? item->GetSlot()    : NULL_SLOT;
    bool  wasEquipped    = (bagSlot == INVENTORY_SLOT_BAG_0 && itemSlot < EQUIPMENT_SLOT_END);

    // Find variants — read-only, no reload (variants were created at designation)
    ItemVariants variants = FindVariants(data.itemEntry);
    uint32 newEntry = (data.tier == TIER_ENCHANTED) ? variants.enchantedEntry : variants.epicEntry;

    if (variants.valid && newEntry)
    {
        // Safety: if the variant template isn't in ObjectMgr yet (first designation
        // in this server session, before next startup), abort the swap so the item
        // is not destroyed without a replacement.
        if (!sObjectMgr->GetItemTemplate(newEntry))
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffffff00[Armory Slot]|r |cff1eff00{}|r has reached tier {}! "
                "Please relog to complete the transformation.",
                itemName, tierName);
            LOG_WARN("module", "[mod-gear-tiers] Tier-up aborted for {} — variant entry {} not in ObjectMgr. Will apply on next login.",
                player->GetName(), newEntry);
            return;
        }

        // Destroy the old item
        if (item)
        {
            player->DestroyItem(bagSlot, itemSlot, true);
            item = nullptr;
        }

        Item* newItem = nullptr;

        if (wasEquipped)
        {
            uint16 equipDest;
            if (player->CanEquipNewItem(itemSlot, equipDest, newEntry, false) == EQUIP_ERR_OK)
                newItem = player->EquipNewItem(equipDest, newEntry, true);
        }

        if (!newItem)
        {
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, newEntry, 1) == EQUIP_ERR_OK)
                newItem = player->StoreNewItem(dest, newEntry, true);
        }

        if (newItem)
        {
            newItem->SetBinding(true);
            newItem->SetState(ITEM_CHANGED, player);
            player->SendNewItem(newItem, 1, false, false);
        }

        data.itemEntry = newEntry;
    }

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffFFD700[Armory Slot]|r |cff1eff00{}|r has morphed into its |cff{}{}|r form!",
        itemName, tierHex, tierName);

    LOG_INFO("module", "[mod-gear-tiers] {} '{}' morphed to {} (entry {})",
        player->GetName(), itemName, tierName, newEntry);
}

// Forward declaration — defined later in the public API section
bool GearTiers_EnsureVariants(uint32 baseEntry, uint32& outEnchanted, uint32& outEpic);

// ---------------------------------------------------------------------------
// Loot tier rolling helpers
// ---------------------------------------------------------------------------

// Returns TIER_NORMAL, TIER_ENCHANTED, or TIER_EPIC for the given item quality.
// Quality 2 (Uncommon): 70% Normal / 25% Enchanted / 5% Epic
// Quality 3 (Rare):     60% Normal / 30% Enchanted / 10% Epic
// Quality 4 (Epic):      0% Normal / 20% Enchanted / 80% Epic
static uint8 RollTierForQuality(uint8 quality)
{
    uint32 roll = urand(1, 100);
    switch (quality)
    {
        case 2: // Uncommon
            if (roll <= 70) return TIER_NORMAL;
            if (roll <= 95) return TIER_ENCHANTED;
            return TIER_EPIC;
        case 3: // Rare
            if (roll <= 60) return TIER_NORMAL;
            if (roll <= 90) return TIER_ENCHANTED;
            return TIER_EPIC;
        case 4: // Epic
            if (roll <= 20) return TIER_ENCHANTED;
            return TIER_EPIC;
        default:
            return TIER_NORMAL;
    }
}

// Checks whether an item qualifies for loot tier rolling.
// Returns false if the item should be skipped (no roll).
static bool ShouldRollTier(ItemTemplate const* proto)
{
    if (!proto)
        return false;

    // Already a Sanctum variant
    if (proto->ItemId >= SANCTUM_ENTRY_MIN)
        return false;

    // Only Uncommon/Rare/Epic — skip white/grey and legendary+
    if (proto->Quality < 2 || proto->Quality > 4)
        return false;

    // Only weapons and armor — skip bags, consumables, gems, etc.
    if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
        return false;

    // Skip bag items
    if (proto->BagFamily != 0)
        return false;

    // Skip deprecated/unequippable items
    if (proto->HasFlag(ITEM_FLAG_DEPRECATED))
        return false;

    // Skip purely cosmetic / no-stat items (no armor, no weapon damage, no stats)
    bool hasStats = (proto->Armor > 0 || proto->Damage[0].DamageMin > 0);
    if (!hasStats)
    {
        for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        {
            if (proto->ItemStat[i].ItemStatValue != 0)
            {
                hasStats = true;
                break;
            }
        }
    }
    if (!hasStats)
        return false;

    return true;
}

// Attempts to apply a random Sanctum tier to a newly looted item.
// Uses only the in-memory g_variantCache — zero DB queries during gameplay.
// Returns true  if the item was considered (rolled or Normal).
// Returns false if the item was skipped entirely (wrong class/quality/etc).
// outPendingRestart is set to true if this item type has no variants yet.
static bool TryApplyLootTier(Player* player, Item* item, bool& outPendingRestart)
{
    outPendingRestart = false;

    if (!player || !item)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!ShouldRollTier(proto))
        return false;

    uint32 baseEntry = proto->ItemId;

    // Cache-only lookup — no DB queries on the hot loot path.
    auto cacheIt = g_variantCache.find(baseEntry);
    if (cacheIt == g_variantCache.end() || !cacheIt->second.valid)
    {
        // Variants don't exist yet. Queue creation via GearTiers_EnsureVariants
        // only from .armoryslot roll (player-triggered, blocking is OK there).
        // For automatic loot rolls, stay Normal until next restart populates the cache.
        outPendingRestart = true;
        return true;
    }

    uint32 enchEntry = cacheIt->second.enchantedEntry;
    uint32 epicEntry = cacheIt->second.epicEntry;

    uint8 rolledTier;
    Map* map = player->GetMap();
    if (map && map->IsRaid())
    {
        // Raid gear: flat 80% Epic / 20% Enchanted (no Normal). "All raid gear 80/20."
        rolledTier = (urand(0, 99) < 20) ? TIER_ENCHANTED : TIER_EPIC;
    }
    else
    {
        rolledTier = RollTierForQuality(proto->Quality);
    }

    // Normal = nothing to do
    if (rolledTier == TIER_NORMAL)
        return true;

    uint32 newEntry = (rolledTier == TIER_ENCHANTED) ? enchEntry : epicEntry;
    if (!newEntry || !sObjectMgr->GetItemTemplate(newEntry))
    {
        LOG_WARN("module", "[mod-gear-tiers] TryApplyLootTier: variant entry {} not in ObjectMgr for base {}", newEntry, baseEntry);
        return true;
    }

    // Capture position before destroying
    uint8 bagSlot  = item->GetBagSlot();
    uint8 itemSlot = item->GetSlot();
    bool wasEquipped = (bagSlot == INVENTORY_SLOT_BAG_0 && itemSlot < EQUIPMENT_SLOT_END);

    std::string itemName = proto->Name1;

    player->DestroyItem(bagSlot, itemSlot, true);
    item = nullptr;

    Item* newItem = nullptr;

    if (wasEquipped)
    {
        uint16 equipDest;
        if (player->CanEquipNewItem(itemSlot, equipDest, newEntry, false) == EQUIP_ERR_OK)
            newItem = player->EquipNewItem(equipDest, newEntry, true);
    }

    if (!newItem)
    {
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, newEntry, 1) == EQUIP_ERR_OK)
            newItem = player->StoreNewItem(dest, newEntry, true);
    }

    if (newItem)
    {
        newItem->SetBinding(true);
        newItem->SetState(ITEM_CHANGED, player);
        player->SendNewItem(newItem, 1, false, false);
    }

    const char* tierHex  = (rolledTier == TIER_ENCHANTED) ? "0070dd" : "a335ee";
    const char* tierName = TierName(rolledTier);

    ChatHandler(player->GetSession()).PSendSysMessage(
        "|cffFFD700[Gear Roll]|r |cff{}{}|r |cff1eff00{}|r",
        tierHex, tierName, itemName);

    LOG_INFO("module", "[mod-gear-tiers] Loot roll: {} looted '{}' → {} (entry {})",
        player->GetName(), itemName, tierName, newEntry);

    return true;
}

// ===========================================================================
// Command Script
// ===========================================================================

class ArmorySlotCommandScript : public CommandScript
{
public:
    ArmorySlotCommandScript() : CommandScript("ArmorySlotCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable armoryTable =
        {
            { "designate",  HandleSetEntry,   SEC_PLAYER,      Console::No  },
            { "clear",      HandleClear,      SEC_PLAYER,      Console::No  },
            { "status",     HandleStatus,     SEC_PLAYER,      Console::No  },
            { "roll",         HandleGearRoll,     SEC_PLAYER,      Console::No  },
            { "prepraids",    HandlePrepRaids,    SEC_GAMEMASTER,  Console::Yes },
            { "prepdungeons", HandlePrepDungeons, SEC_GAMEMASTER,  Console::Yes },
            { "prepall",      HandlePrepAll,      SEC_GAMEMASTER,  Console::Yes }
        };
        static ChatCommandTable commandTable =
        {
            { "armoryslot", armoryTable }
        };
        return commandTable;
    }

    // .armoryslot designate <entryId>
    // Called by the addon when the player drops an item onto the Armory Slot button.
    static bool HandleSetEntry(ChatHandler* handler, std::string_view entryStr)
    {
        // Parse entry ID manually — avoids AzerothCore uint32 arg parser quirks
        uint32 entryId = 0;
        for (char c : entryStr)
        {
            if (c < '0' || c > '9')
            {
                handler->SendSysMessage("|cffFF4444[Armory Slot]|r Usage: .armoryslot designate <item entry id>");
                return true;
            }
            entryId = entryId * 10 + (c - '0');
        }
        if (entryId == 0)
        {
            handler->SendSysMessage("|cffFF4444[Armory Slot]|r Usage: .armoryslot designate <item entry id>");
            return true;
        }

        LOG_INFO("module", "[mod-gear-tiers] HandleSetEntry called, entryId={}", entryId);
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;
        LOG_INFO("module", "[mod-gear-tiers] HandleSetEntry player={}", player->GetName());

        Item* item = player->GetItemByEntry(entryId);
        if (!item)
        {
            handler->SendSysMessage("|cffFF4444[Armory Slot]|r Item not found in your bags. Try picking it up again.");
            return true;
        }

        // Reject grey and white items — green (Uncommon) or higher only
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entryId);
        if (!proto || proto->Quality < ARMORY_MIN_QUALITY)
        {
            handler->SendSysMessage("|cffFF4444[Armory Slot]|r Only Uncommon (green) quality items or higher can be designated.");
            return true;
        }

        uint32 playerGuid = player->GetGUID().GetCounter();

        // Note: re-designation of already-morphed Sanctum variants is handled at next login
        // via OnPlayerLogin which reads tier/gxp from DB. No WorldDatabase sync queries here.

        ArmoryData& data = g_armory[playerGuid];

        // Different base item — reset GXP and tier
        if (data.itemEntry != entryId)
        {
            data.gearXP = 0;
            data.tier   = TIER_NORMAL;
        }

        data.itemEntry = entryId;
        data.dirty     = true;
        SaveArmoryData(playerGuid, data);

        // Variant creation (Enchanted/Epic entries) is handled by GearTiersWorldScript::OnStartup
        // on the next server boot — safe to call LoadItemTemplates() there, not mid-session.

        handler->PSendSysMessage(
            "|cffFFD700[Armory Slot]|r Designated: |cff1eff00{}|r  |cff888888(GXP: {}  Tier: {})|r",
            item->GetTemplate()->Name1, data.gearXP, TierName(data.tier));

        SendArmorySync(player, data);
        return true;
    }

    // .armoryslot clear
    static bool HandleClear(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        uint32 playerGuid = player->GetGUID().GetCounter();
        g_armory.erase(playerGuid);
        CharacterDatabase.Execute("DELETE FROM character_armory_slot WHERE guid = {}", playerGuid);

        handler->SendSysMessage("|cffFFD700[Armory Slot]|r Designation cleared.");
        handler->SendSysMessage("SANCTUMARMORY:CLEAR");
        return true;
    }

    // .armoryslot status
    static bool HandleStatus(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        uint32 playerGuid = player->GetGUID().GetCounter();
        auto it = g_armory.find(playerGuid);

        if (it == g_armory.end() || !it->second.itemEntry)
        {
            handler->SendSysMessage("|cffFFD700[Armory Slot]|r No item designated.");
            return true;
        }

        ArmoryData& data = it->second;
        Item* item       = FindArmoryItem(player, data);
        const char* name = item ? item->GetTemplate()->Name1.c_str() : "(item not in bags)";

        handler->PSendSysMessage("|cffFFD700[Armory Slot] Status|r");
        handler->PSendSysMessage("  Item:  |cff1eff00{}|r", name);
        handler->PSendSysMessage("  GXP:   {}", data.gearXP);
        handler->PSendSysMessage("  Tier:  {}", TierName(data.tier));

        if (data.tier == TIER_NORMAL)
            handler->PSendSysMessage("  Next:  {} / {} GXP for Enchanted",
                data.gearXP, GXP_TIER1_THRESHOLD);
        else if (data.tier == TIER_ENCHANTED)
            handler->PSendSysMessage("  Next:  {} / {} GXP for Epic",
                data.gearXP, GXP_TIER2_THRESHOLD);
        else
            handler->PSendSysMessage("  Tier:  |cffa335eeMAX (Epic)|r");

        return true;
    }

    // .armoryslot roll — retroactively roll Sanctum tiers on all gear in inventory.
    // Blocking DB calls are acceptable here — this is player-triggered, runs once.
    static bool HandleGearRoll(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        uint32 rolled         = 0;
        uint32 skipped        = 0;
        uint32 pendingRestart = 0;

        // Collect all items to roll
        std::vector<Item*> items;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* i = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) items.push_back(i);
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* i = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)) items.push_back(i);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* i = bag->GetItemByPos(static_cast<uint8>(slot))) items.push_back(i);

        for (Item* item : items)
        {
            if (!item) continue;
            ItemTemplate const* proto = item->GetTemplate();
            if (!ShouldRollTier(proto)) { ++skipped; continue; }

            uint32 baseEntry = proto->ItemId;

            // Prime the cache for uncached items — blocking DB call is OK in a command handler.
            if (g_variantCache.find(baseEntry) == g_variantCache.end())
            {
                uint32 ench = 0, epic = 0;
                if (GearTiers_EnsureVariants(baseEntry, ench, epic))
                    g_variantCache[baseEntry] = { ench, epic, true };
            }

            bool pending = false;
            if (TryApplyLootTier(player, item, pending))
            {
                ++rolled;
                if (pending) ++pendingRestart;
            }
            else
                ++skipped;
        }

        handler->PSendSysMessage(
            "|cffFFD700[Gear Roll]|r Rolled {} item(s). {} skipped. {} variant(s) pending server restart.",
            rolled, skipped, pendingRestart);

        return true;
    }

    // .armoryslot prepraids — GM-only, console-safe.
    // Pre-creates Enchanted + Epic variants for every equippable gear item
    // that can drop in any raid instance (via creature_loot_template and
    // reference_loot_template).  Run once after adding raid loot tables.
    // A server restart is required after this command for new variants to
    // be live in ObjectMgr and TryApplyLootTier's cache.
    static bool HandlePrepRaids(ChatHandler* handler)
    {
        // ------------------------------------------------------------------
        // Step 1 — Build the set of raid map IDs from the DBC store.
        // ------------------------------------------------------------------
        std::set<uint32> raidMaps;
        for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
        {
            if (MapEntry const* me = sMapStore.LookupEntry(i))
                if (me->IsRaid())
                    raidMaps.insert(me->MapID);
        }

        if (raidMaps.empty())
        {
            handler->SendSysMessage("[Gear Prep] No raid maps found in DBC store.");
            return true;
        }

        // ------------------------------------------------------------------
        // Step 2 — Build the IN (...) clause.
        // ------------------------------------------------------------------
        std::ostringstream mapList;
        bool firstMap = true;
        for (uint32 mid : raidMaps)
        {
            if (!firstMap) mapList << ",";
            mapList << mid;
            firstMap = false;
        }
        std::string mapInClause = mapList.str();

        // ------------------------------------------------------------------
        // Step 3 — Query DISTINCT equippable gear base entries from raid loot
        //           tables (direct loot + reference loot UNION).
        // ------------------------------------------------------------------
        std::ostringstream querySS;
        querySS <<
            "SELECT DISTINCT it.entry "
            "FROM creature c "
            "JOIN creature_loot_template l ON l.Entry = c.id1 "
            "JOIN item_template it ON it.entry = l.Item "
            "WHERE c.map IN (" << mapInClause << ") "
            "  AND l.Reference = 0 "
            "  AND it.class IN (2,4) "
            "  AND it.InventoryType > 0 "
            "  AND it.Quality BETWEEN 2 AND 4 "
            "  AND it.entry < " << SANCTUM_ENTRY_MIN <<
            " UNION "
            "SELECT DISTINCT it.entry "
            "FROM creature c "
            "JOIN creature_loot_template l ON l.Entry = c.id1 AND l.Reference > 0 "
            "JOIN reference_loot_template r ON r.Entry = l.Reference "
            "JOIN item_template it ON it.entry = r.Item "
            "WHERE c.map IN (" << mapInClause << ") "
            "  AND it.class IN (2,4) "
            "  AND it.InventoryType > 0 "
            "  AND it.Quality BETWEEN 2 AND 4 "
            "  AND it.entry < " << SANCTUM_ENTRY_MIN;

        QueryResult result = WorldDatabase.Query(querySS.str());

        if (!result)
        {
            handler->PSendSysMessage("[Gear Prep] No qualifying raid gear found in loot tables for {} raid map(s). "
                "Nothing to create.", static_cast<uint32>(raidMaps.size()));
            return true;
        }

        // ------------------------------------------------------------------
        // Step 4 — Iterate results; call GetOrCreateVariants for each entry.
        // ------------------------------------------------------------------
        uint32 totalFound    = 0;
        uint32 newlyCreated  = 0;
        uint32 alreadyExisted = 0;

        do
        {
            uint32 baseEntry = (*result)[0].Get<uint32>();
            ++totalFound;

            // Check if variants already exist (avoid redundant DB work)
            QueryResult existing = WorldDatabase.Query(
                "SELECT enchanted_entry, epic_entry FROM sanctum_item_variants WHERE base_entry = {}",
                baseEntry);
            if (existing)
            {
                ++alreadyExisted;
                continue;
            }

            // GetOrCreateVariants logs on missing ObjectMgr entry and returns {} — safe to call
            ItemVariants v = GetOrCreateVariants(baseEntry);
            if (v.valid)
                ++newlyCreated;

        } while (result->NextRow());

        // ------------------------------------------------------------------
        // Step 5 — Summary message.
        // ------------------------------------------------------------------
        handler->PSendSysMessage(
            "[Gear Prep] Raid loot scan complete. "
            "Maps scanned: {}  |  Items found: {}  |  Variants created: {}  |  Already existed: {}. "
            "RESTART REQUIRED to load new variants into memory.",
            static_cast<uint32>(raidMaps.size()), totalFound, newlyCreated, alreadyExisted);

        LOG_INFO("module",
            "[mod-gear-tiers] HandlePrepRaids: {} raid maps, {} items found, {} variants created, {} already existed. Restart required.",
            static_cast<uint32>(raidMaps.size()), totalFound, newlyCreated, alreadyExisted);

        return true;
    }

    // .armoryslot prepall — GM-only, console-safe.
    // COMPREHENSIVE variant generation: creates Enchanted/Epic variants for EVERY
    // equippable base item in item_template (class weapon/armor, equippable,
    // quality 2-4, base entry < SANCTUM_ENTRY_MIN). Replaces the narrow raid/dungeon
    // loot-table scans (which only covered ~27% of gear) so EVERY slot of EVERY class
    // can roll/receive an Epic tier. Run once; RESTART required to load variants into
    // memory; then re-run tools/patch_item_dbc.py so the client has icons.
    static bool HandlePrepAll(ChatHandler* handler)
    {
        // Direct item_template scan — no loot-table joins, so nothing is missed.
        std::ostringstream querySS;
        querySS <<
            "SELECT entry FROM item_template "
            "WHERE class IN (2,4) "
            "  AND InventoryType > 0 "
            "  AND Quality BETWEEN 2 AND 4 "
            "  AND entry < " << SANCTUM_ENTRY_MIN;

        QueryResult result = WorldDatabase.Query(querySS.str());
        if (!result)
        {
            handler->SendSysMessage("[Gear Prep] No equippable base items found. Nothing to create.");
            return true;
        }

        uint32 totalFound     = 0;
        uint32 newlyCreated   = 0;
        uint32 alreadyExisted = 0;

        do
        {
            uint32 baseEntry = (*result)[0].Get<uint32>();
            ++totalFound;

            QueryResult existing = WorldDatabase.Query(
                "SELECT enchanted_entry, epic_entry FROM sanctum_item_variants WHERE base_entry = {}",
                baseEntry);
            if (existing)
            {
                ++alreadyExisted;
                continue;
            }

            ItemVariants v = GetOrCreateVariants(baseEntry);
            if (v.valid)
                ++newlyCreated;

        } while (result->NextRow());

        handler->PSendSysMessage(
            "[Gear Prep] FULL gear scan complete. "
            "Items found: {}  |  Variants created: {}  |  Already existed: {}. "
            "RESTART REQUIRED to load new variants; then re-run tools/patch_item_dbc.py for client icons.",
            totalFound, newlyCreated, alreadyExisted);

        LOG_INFO("module",
            "[mod-gear-tiers] HandlePrepAll: {} items found, {} variants created, {} already existed. Restart + Item.dbc re-patch required.",
            totalFound, newlyCreated, alreadyExisted);

        return true;
    }

    // .armoryslot prepdungeons — GM-only, console-safe.
    // Same as prepraids but for 5-man DUNGEON maps (instance, non-raid).
    // Dungeon loot tiers via the EXISTING quality-based roll (green 70/25/5,
    // blue 60/30/10, epic 0/20/80) — no roll override. This command only
    // pre-creates the Enchanted/Epic variants so dungeon drops have something
    // to tier into. Run once; RESTART required to load variants into memory.
    static bool HandlePrepDungeons(ChatHandler* handler)
    {
        // Step 1 — collect 5-man dungeon map IDs (instance, not raid).
        std::set<uint32> dungeonMaps;
        for (uint32 i = 0; i < sMapStore.GetNumRows(); ++i)
        {
            if (MapEntry const* me = sMapStore.LookupEntry(i))
                if (me->IsDungeon() && !me->IsRaid())
                    dungeonMaps.insert(me->MapID);
        }

        if (dungeonMaps.empty())
        {
            handler->SendSysMessage("[Gear Prep] No dungeon maps found in DBC store.");
            return true;
        }

        // Step 2 — build the IN (...) clause.
        std::ostringstream mapList;
        bool firstMap = true;
        for (uint32 mid : dungeonMaps)
        {
            if (!firstMap) mapList << ",";
            mapList << mid;
            firstMap = false;
        }
        std::string mapInClause = mapList.str();

        // Step 3 — DISTINCT equippable gear from dungeon loot (direct + reference).
        std::ostringstream querySS;
        querySS <<
            "SELECT DISTINCT it.entry "
            "FROM creature c "
            "JOIN creature_loot_template l ON l.Entry = c.id1 "
            "JOIN item_template it ON it.entry = l.Item "
            "WHERE c.map IN (" << mapInClause << ") "
            "  AND l.Reference = 0 "
            "  AND it.class IN (2,4) "
            "  AND it.InventoryType > 0 "
            "  AND it.Quality BETWEEN 2 AND 4 "
            "  AND it.entry < " << SANCTUM_ENTRY_MIN <<
            " UNION "
            "SELECT DISTINCT it.entry "
            "FROM creature c "
            "JOIN creature_loot_template l ON l.Entry = c.id1 AND l.Reference > 0 "
            "JOIN reference_loot_template r ON r.Entry = l.Reference "
            "JOIN item_template it ON it.entry = r.Item "
            "WHERE c.map IN (" << mapInClause << ") "
            "  AND it.class IN (2,4) "
            "  AND it.InventoryType > 0 "
            "  AND it.Quality BETWEEN 2 AND 4 "
            "  AND it.entry < " << SANCTUM_ENTRY_MIN;

        QueryResult result = WorldDatabase.Query(querySS.str());

        if (!result)
        {
            handler->PSendSysMessage("[Gear Prep] No qualifying dungeon gear found in loot tables for {} dungeon map(s). "
                "Nothing to create.", static_cast<uint32>(dungeonMaps.size()));
            return true;
        }

        // Step 4 — create variants for each.
        uint32 totalFound     = 0;
        uint32 newlyCreated   = 0;
        uint32 alreadyExisted = 0;

        do
        {
            uint32 baseEntry = (*result)[0].Get<uint32>();
            ++totalFound;

            QueryResult existing = WorldDatabase.Query(
                "SELECT enchanted_entry, epic_entry FROM sanctum_item_variants WHERE base_entry = {}",
                baseEntry);
            if (existing)
            {
                ++alreadyExisted;
                continue;
            }

            ItemVariants v = GetOrCreateVariants(baseEntry);
            if (v.valid)
                ++newlyCreated;

        } while (result->NextRow());

        // Step 5 — summary.
        handler->PSendSysMessage(
            "[Gear Prep] Dungeon loot scan complete. "
            "Maps scanned: {}  |  Items found: {}  |  Variants created: {}  |  Already existed: {}. "
            "RESTART REQUIRED to load new variants into memory.",
            static_cast<uint32>(dungeonMaps.size()), totalFound, newlyCreated, alreadyExisted);

        LOG_INFO("module",
            "[mod-gear-tiers] HandlePrepDungeons: {} dungeon maps, {} items found, {} variants created, {} already existed. Restart required.",
            static_cast<uint32>(dungeonMaps.size()), totalFound, newlyCreated, alreadyExisted);

        return true;
    }
};

// ===========================================================================
// ---------------------------------------------------------------------------
// Public API — called by mod-aa-system for AA→GXP conversion
// ---------------------------------------------------------------------------
void GearTiers_AddGXP(Player* player, uint32 amount)
{
    if (!player || amount == 0)
        return;

    uint32 playerGuid = player->GetGUID().GetCounter();
    auto it = g_armory.find(playerGuid);
    if (it == g_armory.end() || !it->second.itemEntry)
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffFFD700[Armory Slot]|r No item designated. Use .armoryslot set to designate one first.");
        return;
    }

    ArmoryData& data = it->second;
    if (data.tier >= TIER_EPIC && data.gearXP >= GXP_TIER2_THRESHOLD)
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffFFD700[Armory Slot]|r Your Armory Item is already at max tier.");
        return;
    }

    // Bind on first GXP
    Item* item = FindArmoryItem(player, data);
    if (data.gearXP == 0 && item)
    {
        item->SetBinding(true);
        item->SetState(ITEM_CHANGED, player);
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffFFD700[Armory Slot]|r |cff1eff00{}|r is now bound to you.",
            item->GetTemplate()->Name1);
    }

    data.gearXP += amount;
    data.dirty   = true;
    SaveArmoryData(playerGuid, data);

    if (item)
        CheckTierUpgrade(player, data, item);

    SendArmorySync(player, data);
}

// Player Script
// ===========================================================================

class ArmorySlotPlayerScript : public PlayerScript
{
public:
    ArmorySlotPlayerScript() : PlayerScript("ArmorySlotPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        uint32 playerGuid = player->GetGUID().GetCounter();

        QueryResult result = CharacterDatabase.Query(
            "SELECT item_entry, gear_xp, tier FROM character_armory_slot WHERE guid = {}",
            playerGuid);

        if (!result)
            return;

        Field* fields    = result->Fetch();
        ArmoryData& data = g_armory[playerGuid];
        data.itemEntry   = fields[0].Get<uint32>();
        data.gearXP      = fields[1].Get<uint32>();
        data.tier        = fields[2].Get<uint8>();
        data.dirty       = false;

        RepairArmoryItem(player);
        SendArmorySync(player, data);
    }

    void OnPlayerLogout(Player* player) override
    {
        uint32 playerGuid = player->GetGUID().GetCounter();
        auto it = g_armory.find(playerGuid);
        if (it != g_armory.end())
        {
            if (it->second.dirty)
                SaveArmoryData(playerGuid, it->second);
            g_armory.erase(it);
        }
    }

    void OnPlayerCreatureKill(Player* player, Creature* killed) override
    {
        if (!killed || killed->IsPet() || killed->IsTotem() || killed->IsCritter())
            return;

        CreatureTemplate const* cInfo = killed->GetCreatureTemplate();
        if (!cInfo)
            return;

        uint32 playerGuid = player->GetGUID().GetCounter();
        auto it = g_armory.find(playerGuid);
        if (it == g_armory.end() || !it->second.itemEntry)
            return;

        ArmoryData& data = it->second;
        if (data.tier >= TIER_EPIC && data.gearXP >= GXP_TIER2_THRESHOLD)
            return;

        // Grey mob check
        uint8 playerLevel = player->GetLevel();
        uint8 mobLevel    = killed->GetLevel();
        if (mobLevel <= GetGrayLevel(playerLevel))
            return;

        // Con-based gain
        bool isSpecial = killed->isElite() || (cInfo->rank == CREATURE_ELITE_RARE);
        uint32 gain;
        if (isSpecial)
        {
            gain = GXP_ELITE_KILL;
        }
        else
        {
            int8 levelDiff = (int8)mobLevel - (int8)playerLevel;
            if (levelDiff >= 5)        gain = GXP_RED_KILL;
            else if (levelDiff >= -4)  gain = GXP_YELLOW_KILL;
            else                       gain = GXP_GREEN_KILL;
        }

        // Raid cap
        if (player->GetMap()->IsRaid())
            gain = GXP_RAID_KILL;

        // Bind on first GXP
        Item* item = FindArmoryItem(player, data);
        if (data.gearXP == 0 && item)
        {
            item->SetBinding(true);
            item->SetState(ITEM_CHANGED, player);
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFFD700[Armory Slot]|r |cff1eff00{}|r is now bound to you.",
                item->GetTemplate()->Name1);
        }

        data.gearXP += gain;
        data.dirty   = true;

        // Save immediately so GXP survives server restarts without a clean logout.
        // CharacterDatabase.Execute is async — no performance cost on a solo server.
        SaveArmoryData(playerGuid, data);

        if (item)
            CheckTierUpgrade(player, data, item);

        SendArmorySync(player, data);
    }

    void OnPlayerJustDied(Player* player) override
    {
        RepairArmoryItem(player);

        uint32 playerGuid = player->GetGUID().GetCounter();
        auto it = g_armory.find(playerGuid);
        if (it != g_armory.end() && it->second.dirty)
            SaveArmoryData(playerGuid, it->second);
    }

    // Fires after a player successfully stores a new item from loot (includes master loot).
    // This is the best hook for automatic tier rolling on gear drops.
    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/) override
    {
        bool pending = false;
        TryApplyLootTier(player, item, pending);
        // No need to message for pending — .gear roll can be used retroactively after restart.
    }

    // Fires at the end of Player::RewardQuest, after reward items are already stored
    // (and TryApplyLootTier may have already run on them via OnPlayerStoreNewItem).
    // For any EPIC-quality gear reward in the quest we guarantee the player ends up
    // holding the Epic variant — not the base or the Enchanted variant that a bad roll
    // may have produced.  Non-epic quest rewards are left alone.
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (!player || !quest)
            return;

        // Build a combined list: fixed rewards first, then choice rewards.
        // QuestDef.h: QUEST_REWARDS_COUNT = 4, QUEST_REWARD_CHOICES_COUNT = 6
        // Members: RewardItemId[QUEST_REWARDS_COUNT], RewardChoiceItemId[QUEST_REWARD_CHOICES_COUNT]
        uint32 rewardEntries[QUEST_REWARDS_COUNT + QUEST_REWARD_CHOICES_COUNT];
        uint32 count = 0;
        for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
            rewardEntries[count++] = quest->RewardItemId[i];
        for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
            rewardEntries[count++] = quest->RewardChoiceItemId[i];

        for (uint32 i = 0; i < count; ++i)
        {
            uint32 baseEntry = rewardEntries[i];
            if (!baseEntry)
                continue;

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(baseEntry);
            if (!proto)
                continue;

            // Only force Epic quality turn-ins to Epic.  Lower-quality rewards keep
            // whatever tier TryApplyLootTier already assigned them.
            if (proto->Quality != 4) // 4 = ITEM_QUALITY_EPIC
                continue;

            if (!ShouldRollTier(proto))
                continue;

            // Look up variants from the in-memory cache.
            auto cacheIt = g_variantCache.find(baseEntry);
            if (cacheIt == g_variantCache.end() || !cacheIt->second.valid)
                continue;

            uint32 enchEntry = cacheIt->second.enchantedEntry;
            uint32 epicEntry = cacheIt->second.epicEntry;

            if (!epicEntry || !sObjectMgr->GetItemTemplate(epicEntry))
                continue;

            // If the player already holds the Epic variant, nothing to do.
            if (player->HasItemCount(epicEntry, 1, false))
                continue;

            // Check whether the player is holding the base or the Enchanted variant
            // (the loot hook may have swapped base → enchantedEntry on a bad roll).
            uint32 wrongEntry = 0;
            if (baseEntry && player->HasItemCount(baseEntry, 1, false))
                wrongEntry = baseEntry;
            else if (enchEntry && player->HasItemCount(enchEntry, 1, false))
                wrongEntry = enchEntry;

            if (!wrongEntry)
                continue;

            // Swap wrongEntry out and give the Epic variant.
            player->DestroyItemCount(wrongEntry, 1, true, false);

            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, epicEntry, 1) == EQUIP_ERR_OK)
            {
                Item* ni = player->StoreNewItem(dest, epicEntry, true);
                if (ni)
                {
                    ni->SetBinding(true);
                    ni->SetState(ITEM_CHANGED, player);
                    player->SendNewItem(ni, 1, false, false);
                }
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffFFD700[Gear]|r Turn-in reward set to |cffa335eeEpic|r quality.");

                LOG_INFO("module",
                    "[mod-gear-tiers] OnPlayerCompleteQuest: {} quest reward entry {} forced to Epic (entry {})",
                    player->GetName(), baseEntry, epicEntry);
            }
            else
            {
                LOG_WARN("module",
                    "[mod-gear-tiers] OnPlayerCompleteQuest: {} could not store Epic variant {} — inventory full?",
                    player->GetName(), epicEntry);
            }
        }
    }
};

// ===========================================================================
// World Script — pre-loads all variant item templates at startup
// ===========================================================================

class GearTiersWorldScript : public WorldScript
{
public:
    GearTiersWorldScript() : WorldScript("GearTiersWorldScript") {}

    // OnStartup fires after all DBCs and databases are loaded, before players connect.
    // Safe to call LoadItemTemplates() here — no active sessions, no race conditions.
    void OnStartup() override
    {
        // Guarantee custom tables exist before querying them.
        // OnStartup() is the earliest safe point for synchronous WorldDatabase calls.
        // The async Execute() in AddSC may not have committed yet when we get here.
        WorldDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS sanctum_item_variants ("
            "  base_entry      INT UNSIGNED NOT NULL, "
            "  enchanted_entry INT UNSIGNED NOT NULL DEFAULT 0, "
            "  epic_entry      INT UNSIGNED NOT NULL DEFAULT 0, "
            "  PRIMARY KEY (base_entry)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        WorldDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS sanctum_quality_bonuses_applied ("
            "  id TINYINT UNSIGNED NOT NULL DEFAULT 1, "
            "  applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
            "  PRIMARY KEY (id)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        // -----------------------------------------------------------------------
        // One-time quality-tier stat bonus pass
        // Scales stats, weapon damage, and armor on all base game items by quality.
        // Uncommon +5%, Rare +12%, Epic +25%, Legendary +35%
        // Tracked via sanctum_quality_bonuses_applied — runs exactly once ever.
        // -----------------------------------------------------------------------
        {
            QueryResult bonusCheck = WorldDatabase.Query(
                "SELECT 1 FROM sanctum_quality_bonuses_applied LIMIT 1");
            if (!bonusCheck)
            {
                LOG_INFO("module", "[mod-gear-tiers] Applying quality-tier stat bonuses (one-time pass)...");

                struct QualityTier { uint32 quality; float mult; const char* label; };
                QualityTier tiers[] = {
                    { 2, 1.05f, "Uncommon (+5%)"   },
                    { 3, 1.12f, "Rare (+12%)"      },
                    { 4, 1.25f, "Epic (+25%)"      },
                    { 5, 1.35f, "Legendary (+35%)" }
                };

                for (auto& t : tiers)
                {
                    std::ostringstream sql;
                    sql << "UPDATE item_template SET "
                        << "stat_value1  = ROUND(stat_value1  * " << t.mult << "),"
                        << "stat_value2  = ROUND(stat_value2  * " << t.mult << "),"
                        << "stat_value3  = ROUND(stat_value3  * " << t.mult << "),"
                        << "stat_value4  = ROUND(stat_value4  * " << t.mult << "),"
                        << "stat_value5  = ROUND(stat_value5  * " << t.mult << "),"
                        << "stat_value6  = ROUND(stat_value6  * " << t.mult << "),"
                        << "stat_value7  = ROUND(stat_value7  * " << t.mult << "),"
                        << "stat_value8  = ROUND(stat_value8  * " << t.mult << "),"
                        << "stat_value9  = ROUND(stat_value9  * " << t.mult << "),"
                        << "stat_value10 = ROUND(stat_value10 * " << t.mult << "),"
                        << "dmg_min1     = ROUND(dmg_min1     * " << t.mult << "),"
                        << "dmg_max1     = ROUND(dmg_max1     * " << t.mult << "),"
                        << "dmg_min2     = ROUND(dmg_min2     * " << t.mult << "),"
                        << "dmg_max2     = ROUND(dmg_max2     * " << t.mult << "),"
                        << "armor        = ROUND(armor        * " << t.mult << ") "
                        << "WHERE Quality = " << t.quality
                        << " AND entry < " << SANCTUM_ENTRY_MIN;
                    WorldDatabase.DirectExecute(sql.str());
                    LOG_INFO("module", "[mod-gear-tiers] Quality pass: {}", t.label);
                }

                WorldDatabase.DirectExecute(
                    "INSERT INTO sanctum_quality_bonuses_applied (id) VALUES (1)");
                // Do NOT call LoadItemTemplates() here — it crashes this AzerothCore build
                // when called from OnStartup(). The DB values are now updated; the scaled
                // stats will load normally on the next server restart.
                LOG_INFO("module", "[mod-gear-tiers] Quality bonus pass complete. Restart to apply in memory.");
            }
        }

        // Find every item entry currently designated in character_armory_slot
        QueryResult designated = CharacterDatabase.Query(
            "SELECT DISTINCT item_entry FROM character_armory_slot WHERE item_entry > 0");
        if (!designated)
            return;

        bool createdAny = false;
        do
        {
            uint32 baseEntry = (*designated)[0].Get<uint32>();

            // Skip if already a Sanctum variant (use the original base entry instead)
            QueryResult isVariant = WorldDatabase.Query(
                "SELECT base_entry FROM sanctum_item_variants "
                "WHERE enchanted_entry = {} OR epic_entry = {}", baseEntry, baseEntry);
            if (isVariant)
                baseEntry = (*isVariant)[0].Get<uint32>();

            // Check if variants already exist for this base entry
            QueryResult existing = WorldDatabase.Query(
                "SELECT 1 FROM sanctum_item_variants WHERE base_entry = {}", baseEntry);
            if (existing)
                continue;

            // Create missing variants
            if (!sObjectMgr->GetItemTemplate(baseEntry))
                continue;

            QueryResult maxRes = WorldDatabase.Query(
                "SELECT COALESCE(MAX(entry), {}) FROM item_template WHERE entry >= {}",
                SANCTUM_ENTRY_MIN - 1, SANCTUM_ENTRY_MIN);
            uint32 base      = maxRes ? (*maxRes)[0].Get<uint32>() + 1 : SANCTUM_ENTRY_MIN;
            uint32 enchEntry = base;
            uint32 epicEntry = base + 1;

            CreateVariantInDB(baseEntry, enchEntry, "Enchanted ", 3, 1.4f);
            CreateVariantInDB(baseEntry, epicEntry, "Epic ",      4, 2.0f);
            WorldDatabase.DirectExecute(
                "INSERT INTO sanctum_item_variants (base_entry, enchanted_entry, epic_entry) "
                "VALUES ({}, {}, {})", baseEntry, enchEntry, epicEntry);

            createdAny = true;
            LOG_INFO("module", "[mod-gear-tiers] OnStartup: created variants {} / {} for base {}",
                enchEntry, epicEntry, baseEntry);

        } while (designated->NextRow());

        if (createdAny)
        {
            // Do NOT call LoadItemTemplates() — crashes this AzerothCore build from OnStartup().
            // New variants are in DB; they will be live after the next restart.
            LOG_INFO("module", "[mod-gear-tiers] OnStartup: variants written to DB. Restart to load them.");
        }

        LOG_INFO("module", "[mod-gear-tiers] OnStartup: all armory variants ready.");

        // Populate the loot-roll variant cache from all known variants in DB.
        // This allows TryApplyLootTier to roll without any DB queries at runtime.
        QueryResult allVariants = WorldDatabase.Query(
            "SELECT base_entry, enchanted_entry, epic_entry FROM sanctum_item_variants");
        if (allVariants)
        {
            uint32 cacheCount = 0;
            do
            {
                Field* f = allVariants->Fetch();
                uint32 base  = f[0].Get<uint32>();
                uint32 ench  = f[1].Get<uint32>();
                uint32 epic  = f[2].Get<uint32>();
                if (sObjectMgr->GetItemTemplate(ench) && sObjectMgr->GetItemTemplate(epic))
                {
                    g_variantCache[base] = { ench, epic, true };
                    ++cacheCount;
                }
            } while (allVariants->NextRow());
            LOG_INFO("module", "[mod-gear-tiers] Loot roll cache loaded: {} base entries ready.", cacheCount);
        }
    }
};

// ===========================================================================
// Public cross-module API
// ===========================================================================

// Creates Enchanted/Epic variants for baseEntry in DB if they don't exist.
// Fills outEnchanted / outEpic with the variant item entry IDs.
// Returns true  if both variants are in ObjectMgr right now (safe to give immediately).
// Returns false if variants were just created — a server restart is required to use them.
bool GearTiers_EnsureVariants(uint32 baseEntry, uint32& outEnchanted, uint32& outEpic)
{
    outEnchanted = 0;
    outEpic      = 0;

    ItemVariants v = GetOrCreateVariants(baseEntry);
    if (!v.valid)
        return false;

    outEnchanted = v.enchantedEntry;
    outEpic      = v.epicEntry;

    return sObjectMgr->GetItemTemplate(v.enchantedEntry) != nullptr
        && sObjectMgr->GetItemTemplate(v.epicEntry)      != nullptr;
}

// ===========================================================================
// Registration
// ===========================================================================

void AddSC_mod_gear_tiers()
{
    new ArmorySlotCommandScript();
    new ArmorySlotPlayerScript();
    new GearTiersWorldScript();

    // Queue table creation — these are async and may not be committed before OnStartup()
    // fires, which is fine: OnStartup() re-creates them synchronously at its top.
    WorldDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS sanctum_item_variants ("
        "  base_entry      INT UNSIGNED NOT NULL, "
        "  enchanted_entry INT UNSIGNED NOT NULL DEFAULT 0, "
        "  epic_entry      INT UNSIGNED NOT NULL DEFAULT 0, "
        "  PRIMARY KEY (base_entry)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    WorldDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS sanctum_quality_bonuses_applied ("
        "  id TINYINT UNSIGNED NOT NULL DEFAULT 1, "
        "  applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
        "  PRIMARY KEY (id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    LOG_INFO("module", "[mod-gear-tiers] Module loaded.");
}
