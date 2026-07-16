// mod-power-stones.cpp
//
// Sanctum Power Stones — PHASE 2 (catalog + collection + buy/upgrade) +
// PHASE 3 (socketing + stat application).
// -----------------------------------------------------------------------
// This is the WoW gem-socket replacement system (see CLAUDE.md "Power Stone
// System"). Phase 2 scope:
//   - Hardcoded stone catalog (5 types x 5 tiers x 3 ranks -> stat value)
//   - Player collection (character_power_stones — owned stone instances)
//   - .stone buy / upgrade / list / catalog commands, spending Conquest
//     Shards via mod-conquest-shards' public Shards_TrySpend API
//
// Phase 3 scope (added on top of Phase 2, ALL Phase 2 code preserved):
//   - Socketing owned stones into equipped gear (character_socketed_stones)
//   - Applying socketed stone stats to the player (Crimson/Obsidian/Jade/Iron
//     armor%) via RecomputeStoneStats — recompute-from-scratch, mirrors
//     mod-aa-system's ApplyAAStat add/remove pattern
//   - Surviving a gear tier-up (mod-gear-tiers destroys+recreates the item
//     with a new guid) via PowerStones_OnItemMorphed
//   - .stone socket / unsocket / sockets commands
//
// EXPLICITLY OUT OF SCOPE for Phase 3 (later phases):
//   - Weapon procs
//   - Amber "universal power" damage/healing application (stored only)
//   - Iron's flat phys-DR secondary value (stored only, not applied)
//   - Awakenings / Attunements
//   - VOID (lifesteal) stone type — deferred per design doc, not buyable yet
//   - Gold fee for unsocketing (Phase 2/3 unsocket is free; a tiered gold
//     swap fee is a later phase)
//
// Stone types (stored as TINYINT in character_power_stones.stone_type):
//   1 = CRIMSON  (% Crit)
//   2 = OBSIDIAN (% Hit)
//   3 = JADE     (flat Spirit)
//   4 = IRON     (% Armor, + flat % physical DR at T4/T5)
//   5 = AMBER    (% universal power — damage AND healing)
//
// Value formula: value = tierCeiling[type][tier] * rankPct[rank]
//   rankPct: R1=60%, R2=80%, R3=100% (R3 == the tier's ceiling)
//
// Cost curve (shared by all 5 stone types):
//   Buy-in (new T1 R1 stone):            5 shards
//   Rank-up cost by tier (R1->R2, R2->R3):
//     T1: 2, 3   T2: 4, 7   T3: 10, 14   T4: 21, 32   T5: 47, 70
//   Tier-up cost (R3 -> next tier R1):
//     ->T2: 11   ->T3: 24   ->T4: 53   ->T5: 117
//
// See memory/project_power_stones.md for the full design doc.

#include "ScriptMgr.h"
#include "Player.h"
#include "Item.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "CommandScript.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Creature.h"
#include "GossipDef.h"
#include "ScriptedGossip.h"
#include "Log.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdio>
#include <cctype>
#include <cmath>

using namespace Acore::ChatCommands;

// ---------------------------------------------------------------------------
// Extern from mod-conquest-shards — same cross-module pattern mod-aa-system
// uses for GearTiers_AddGXP (extern-declare, no cross-module include path).
// ---------------------------------------------------------------------------
extern int64 Shards_GetBalance(uint32 lowGuid);
extern bool  Shards_TrySpend(Player* player, uint32 amount, char const* reason);

// ---------------------------------------------------------------------------
// Stone type enum
// ---------------------------------------------------------------------------

enum SanctumStoneType : uint8
{
    STONE_NONE     = 0,
    STONE_CRIMSON  = 1, // % Crit
    STONE_OBSIDIAN = 2, // % Hit
    STONE_JADE     = 3, // flat Spirit
    STONE_IRON     = 4, // % Armor (+ flat % phys DR at T4/T5)
    STONE_AMBER    = 5  // % universal power (damage + healing)

    // STONE_VOID (Lifesteal) is deferred per design doc — NOT buyable yet.
    // Reserve a future value (6) for it once it's designed/approved.
};

static constexpr uint8 STONE_TYPE_MIN = STONE_CRIMSON;
static constexpr uint8 STONE_TYPE_MAX = STONE_AMBER;
static constexpr uint8 STONE_TIER_MIN = 1;
static constexpr uint8 STONE_TIER_MAX = 5;
static constexpr uint8 STONE_RANK_MIN = 1;
static constexpr uint8 STONE_RANK_MAX = 3;

static char const* StoneTypeName(uint8 type)
{
    switch (type)
    {
        case STONE_CRIMSON:  return "Crimson";
        case STONE_OBSIDIAN: return "Obsidian";
        case STONE_JADE:     return "Jade";
        case STONE_IRON:     return "Iron";
        case STONE_AMBER:    return "Amber";
        default:             return "Unknown";
    }
}

// Parses a stone type by name, case-insensitive. Returns STONE_NONE if no match.
static uint8 ParseStoneTypeByName(std::string_view argStr)
{
    std::string s(argStr);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (s == "crimson")  return STONE_CRIMSON;
    if (s == "obsidian") return STONE_OBSIDIAN;
    if (s == "jade")     return STONE_JADE;
    if (s == "iron")     return STONE_IRON;
    if (s == "amber")    return STONE_AMBER;
    return STONE_NONE;
}

// ---------------------------------------------------------------------------
// Catalog data (fixed game data — hardcoded, tuned via this table only)
// ---------------------------------------------------------------------------

// Tier ceilings (the Rank 3 value at that tier), indexed [tier-1].
static constexpr double g_crimsonCeiling[5]  = { 1.5, 3.0, 4.5, 6.5, 9.0 };   // % crit
static constexpr double g_obsidianCeiling[5] = { 1.5, 3.0, 4.5, 6.5, 9.0 };   // % hit
static constexpr double g_jadeCeiling[5]     = { 25.0, 45.0, 70.0, 105.0, 150.0 }; // flat Spirit
static constexpr double g_ironCeiling[5]     = { 2.0, 4.0, 7.0, 10.0, 14.0 }; // % armor
static constexpr double g_ironPhysDR[5]      = { 0.0, 0.0, 0.0, 0.5, 1.0 };   // flat % phys DR (secondary)
static constexpr double g_amberCeiling[5]    = { 1.0, 2.0, 3.0, 4.0, 5.0 };   // % universal power

// Rank scaling: displayed value = ceiling[tier] * rankPct[rank]
static constexpr double g_rankPct[4] = { 0.0, 0.60, 0.80, 1.00 }; // index by rank (1..3); [0] unused

// Shard cost curve
static constexpr uint32 STONE_BUY_IN_COST = 5;

// Rank-up cost [tier-1][0]=R1->R2, [1]=R2->R3
static constexpr uint32 g_rankUpCost[5][2] =
{
    { 2, 3 },   // T1
    { 4, 7 },   // T2
    { 10, 14 }, // T3
    { 21, 32 }, // T4
    { 47, 70 }  // T5
};

// Tier-up cost to REACH tier N (index by target tier, [0] unused, [1]=T1 n/a)
// g_tierUpCost[targetTier] where targetTier in {2,3,4,5}
static constexpr uint32 g_tierUpCost[6] = { 0, 0, 11, 24, 53, 117 };

// PHASE 4 — gold swap fee (GDD "5g through 250g by tier"). Charged on UNSOCKET
// only; socketing an empty socket stays free (the stone was already paid for in
// Conquest Shards). Indexed by stone tier (1..5), value in COPPER. Locked ladder
// 2026-07-15: 5g / 25g / 60g / 130g / 250g.
static constexpr uint32 g_unsocketGoldCost[6] =
{
    0,        // [0] unused
    50000,    // T1  =   5g
    250000,   // T2  =  25g
    600000,   // T3  =  60g
    1300000,  // T4  = 130g
    2500000   // T5  = 250g
};

// Copper fee to unsocket a stone of the given tier. Out-of-range tiers -> 0
// (fail-safe: never charge for an unrecognized tier).
static uint32 StoneUnsocketGoldCost(uint8 tier)
{
    if (tier < STONE_TIER_MIN || tier > STONE_TIER_MAX)
        return 0;
    return g_unsocketGoldCost[tier];
}

// Computes the display value + unit label for a (type, tier, rank).
// For IRON, the primary value is % armor; the secondary flat phys-DR bonus is
// returned via outSecondary/outSecondaryUnit (empty unit = not applicable).
static double GetStoneStatValue(uint8 type, uint8 tier, uint8 rank, char const*& outUnit,
                                 double& outSecondary, char const*& outSecondaryUnit)
{
    outSecondary = 0.0;
    outSecondaryUnit = "";

    if (tier < STONE_TIER_MIN || tier > STONE_TIER_MAX || rank < STONE_RANK_MIN || rank > STONE_RANK_MAX)
    {
        outUnit = "";
        return 0.0;
    }

    double ceiling = 0.0;
    switch (type)
    {
        case STONE_CRIMSON:  ceiling = g_crimsonCeiling[tier - 1];  outUnit = "% crit";  break;
        case STONE_OBSIDIAN: ceiling = g_obsidianCeiling[tier - 1]; outUnit = "% hit";   break;
        case STONE_JADE:     ceiling = g_jadeCeiling[tier - 1];     outUnit = " Spirit"; break;
        case STONE_IRON:
            ceiling = g_ironCeiling[tier - 1];
            outUnit = "% armor";
            outSecondary = g_ironPhysDR[tier - 1] * g_rankPct[rank];
            outSecondaryUnit = "% phys DR";
            break;
        case STONE_AMBER:    ceiling = g_amberCeiling[tier - 1];    outUnit = "% power"; break;
        default:              outUnit = ""; return 0.0;
    }

    return ceiling * g_rankPct[rank];
}

// Formats a stat value for display: Jade (flat Spirit) rounds to nearest
// integer, everything else rounds to 1 decimal.
static std::string FormatStoneValue(uint8 type, double value)
{
    char buf[32];
    if (type == STONE_JADE)
        std::snprintf(buf, sizeof(buf), "%.0f", value);
    else
        std::snprintf(buf, sizeof(buf), "%.1f", value);
    return std::string(buf);
}

// Determines the next upgrade step for a stone currently at (tier, rank).
// Returns true if an upgrade is possible (not already maxed), filling in the
// shard cost and the resulting (newTier, newRank). Returns false if the stone
// is already at T5 R3 (fully maxed) — cost/newTier/newRank left untouched.
static bool GetNextUpgradeStep(uint8 tier, uint8 rank, uint32& outCost, uint8& outNewTier, uint8& outNewRank)
{
    if (rank < STONE_RANK_MAX)
    {
        // Rank-up within the current tier.
        outCost = g_rankUpCost[tier - 1][rank - 1]; // rank=1 -> index0 (R1->R2), rank=2 -> index1 (R2->R3)
        outNewTier = tier;
        outNewRank = rank + 1;
        return true;
    }

    if (tier < STONE_TIER_MAX)
    {
        // Tier-up: stone resets to Rank 1 of the next tier.
        uint8 nextTier = tier + 1;
        outCost = g_tierUpCost[nextTier];
        outNewTier = nextTier;
        outNewRank = STONE_RANK_MIN;
        return true;
    }

    // Already T5 R3 — fully maxed.
    return false;
}

// ---------------------------------------------------------------------------
// Player collection cache
// ---------------------------------------------------------------------------

struct OwnedStone
{
    uint64 id;
    uint8  type;
    uint8  tier;
    uint8  rank;
};

// Keyed by low-part GUID. Loaded lazily on login, dropped on logout (rows are
// always persisted immediately on every change — same pattern as
// mod-conquest-shards' g_shardBalance cache).
static std::unordered_map<uint32, std::vector<OwnedStone>> g_ownedStones;

static std::vector<OwnedStone> LoadOwnedStonesFromDB(uint32 lowGuid)
{
    std::vector<OwnedStone> stones;
    // `rank` is a reserved keyword in MySQL 8 (window-function RANK()) — it MUST
    // be backtick-quoted in every query or the statement fails with ERROR 1064.
    QueryResult result = CharacterDatabase.Query(
        "SELECT id, stone_type, tier, `rank` FROM character_power_stones WHERE guid = {}", lowGuid);
    if (!result)
        return stones;

    do
    {
        Field* fields = result->Fetch();
        OwnedStone s;
        s.id   = fields[0].Get<uint64>();
        s.type = fields[1].Get<uint8>();
        s.tier = fields[2].Get<uint8>();
        s.rank = fields[3].Get<uint8>();
        stones.push_back(s);
    } while (result->NextRow());

    return stones;
}

static std::vector<OwnedStone>& GetCachedStones(uint32 lowGuid)
{
    auto it = g_ownedStones.find(lowGuid);
    if (it != g_ownedStones.end())
        return it->second;

    return g_ownedStones.emplace(lowGuid, LoadOwnedStonesFromDB(lowGuid)).first->second;
}

// Inserts a new owned stone row, returning the new row's id.
// Uses DirectExecute (SYNCHRONOUS — the row is committed before we read it back)
// then a targeted SELECT of the newest row for this guid. LAST_INSERT_ID() is
// NOT reliable here: an async Execute + separate synchronous Query run on
// different pooled connections, so the id would come back 0. On a solo,
// single-player server "newest row for this guid" is unambiguous.
// Note: uint8 args are cast to uint32 before formatting — the DB layer's fmt
// backend prints an unmodified uint8 (== unsigned char) as a text glyph, not a
// number, which would corrupt stone_type/tier/rank.
static uint64 InsertOwnedStone(uint32 lowGuid, uint8 type, uint8 tier, uint8 rank)
{
    CharacterDatabase.DirectExecute(
        "INSERT INTO character_power_stones (guid, stone_type, tier, `rank`) VALUES ({}, {}, {}, {})",
        lowGuid, static_cast<uint32>(type), static_cast<uint32>(tier), static_cast<uint32>(rank));

    QueryResult result = CharacterDatabase.Query(
        "SELECT id FROM character_power_stones WHERE guid = {} ORDER BY id DESC LIMIT 1", lowGuid);
    if (!result)
        return 0;

    return result->Fetch()[0].Get<uint64>();
}

static void UpdateOwnedStone(uint64 id, uint8 newTier, uint8 newRank)
{
    // DirectExecute (SYNCHRONOUS): .stone upgrade calls RecomputeStoneStats right
    // after this, which re-reads the stone's tier/rank via a sync JOIN. An async
    // Execute loses that race and the recompute applies the OLD tier/rank. See the
    // note in InsertSocketRow.
    // uint8 -> uint32 cast: see the note in InsertOwnedStone.
    CharacterDatabase.DirectExecute(
        "UPDATE character_power_stones SET tier = {}, `rank` = {} WHERE id = {}",
        static_cast<uint32>(newTier), static_cast<uint32>(newRank), id);
}

// ===========================================================================
// PHASE 3 — Socketing model
// ===========================================================================

// Interim socket-count proxy: derived from item quality. A future version
// can instead read mod-gear-tiers' real Normal/Enchanted/Epic tier data for
// the Armory item specifically, but Phase 3 needs a rule that works for ANY
// equipped item, not just the Armory Slot item, so quality is the stand-in.
static uint8 SocketCountForItem(Item* item)
{
    if (!item)
        return 0;

    uint32 quality = item->GetTemplate()->Quality;
    if (quality >= ITEM_QUALITY_EPIC)
        return 3;
    if (quality >= ITEM_QUALITY_RARE)
        return 2;
    return 1;
}

// Parses an equip-slot name (case-insensitive) to an EQUIPMENT_SLOT_* value.
// Returns -1 if unrecognized.
static int ParseEquipSlotByName(std::string_view argStr)
{
    std::string s(argStr);
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (s == "head")                             return EQUIPMENT_SLOT_HEAD;
    if (s == "neck")                             return EQUIPMENT_SLOT_NECK;
    if (s == "shoulder" || s == "shoulders")      return EQUIPMENT_SLOT_SHOULDERS;
    if (s == "back" || s == "cloak")              return EQUIPMENT_SLOT_BACK;
    if (s == "chest")                             return EQUIPMENT_SLOT_CHEST;
    if (s == "wrist" || s == "wrists" || s == "bracers") return EQUIPMENT_SLOT_WRISTS;
    if (s == "hands" || s == "gloves")            return EQUIPMENT_SLOT_HANDS;
    if (s == "waist" || s == "belt")              return EQUIPMENT_SLOT_WAIST;
    if (s == "legs")                              return EQUIPMENT_SLOT_LEGS;
    if (s == "feet" || s == "boots")              return EQUIPMENT_SLOT_FEET;
    if (s == "finger1" || s == "ring1")           return EQUIPMENT_SLOT_FINGER1;
    if (s == "finger2" || s == "ring2")           return EQUIPMENT_SLOT_FINGER2;
    if (s == "trinket1")                          return EQUIPMENT_SLOT_TRINKET1;
    if (s == "trinket2")                          return EQUIPMENT_SLOT_TRINKET2;
    if (s == "mainhand" || s == "mh")             return EQUIPMENT_SLOT_MAINHAND;
    if (s == "offhand" || s == "oh")              return EQUIPMENT_SLOT_OFFHAND;
    if (s == "ranged")                            return EQUIPMENT_SLOT_RANGED;

    return -1;
}

// ---------------------------------------------------------------------------
// character_socketed_stones DB access
// ---------------------------------------------------------------------------

struct SocketedStone
{
    uint32 itemGuid;
    uint8  socketIndex;
    uint64 stoneId;
    uint8  type;
    uint8  tier;
    uint8  rank;
};

// Loads every socketed stone row for this character, joined to the catalog
// row for type/tier/rank. `rank` is backtick-quoted (MySQL 8 reserved word)
// exactly as the Phase 2 load query does.
static std::vector<SocketedStone> LoadSocketedStonesFromDB(uint32 lowGuid)
{
    std::vector<SocketedStone> rows;
    QueryResult result = CharacterDatabase.Query(
        "SELECT css.item_guid, css.socket_index, css.stone_id, cps.stone_type, cps.tier, cps.`rank` "
        "FROM character_socketed_stones css "
        "INNER JOIN character_power_stones cps ON cps.id = css.stone_id "
        "WHERE css.guid = {}", lowGuid);
    if (!result)
        return rows;

    do
    {
        Field* fields = result->Fetch();
        SocketedStone s;
        s.itemGuid    = fields[0].Get<uint32>();
        s.socketIndex = fields[1].Get<uint8>();
        s.stoneId     = fields[2].Get<uint64>();
        s.type        = fields[3].Get<uint8>();
        s.tier        = fields[4].Get<uint8>();
        s.rank        = fields[5].Get<uint8>();
        rows.push_back(s);
    } while (result->NextRow());

    return rows;
}

// Returns the socketed rows for a single equipped item (item_guid match only —
// no join needed since only slot occupancy matters here).
static std::vector<std::pair<uint8 /*socketIndex*/, uint64 /*stoneId*/>> LoadSocketsForItem(uint32 itemGuidLow)
{
    std::vector<std::pair<uint8, uint64>> rows;
    QueryResult result = CharacterDatabase.Query(
        "SELECT socket_index, stone_id FROM character_socketed_stones WHERE item_guid = {}", itemGuidLow);
    if (!result)
        return rows;

    do
    {
        Field* fields = result->Fetch();
        rows.emplace_back(fields[0].Get<uint8>(), fields[1].Get<uint64>());
    } while (result->NextRow());

    return rows;
}

static bool IsStoneSocketed(uint32 lowGuid, uint64 stoneId)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM character_socketed_stones WHERE guid = {} AND stone_id = {} LIMIT 1", lowGuid, stoneId);
    return result != nullptr;
}

static void InsertSocketRow(uint32 lowGuid, uint32 itemGuidLow, uint8 socketIndex, uint64 stoneId)
{
    // DirectExecute (SYNCHRONOUS): RecomputeStoneStats reads the socket table via
    // a sync Query IMMEDIATELY after this write. An async Execute is only enqueued
    // to a worker thread, so the recompute's sync read (on a different connection)
    // wins the race and sees the PRE-socket state — the stats end up one action
    // behind, which reads as an exact sign inversion. Same DirectExecute rule the
    // module already uses in InsertOwnedStone / PowerStones_OnItemMorphed.
    // uint8 -> uint32 cast: see the note in InsertOwnedStone (fmt prints a raw
    // uint8 as a text glyph, not a number).
    CharacterDatabase.DirectExecute(
        "INSERT INTO character_socketed_stones (guid, item_guid, socket_index, stone_id) VALUES ({}, {}, {}, {})",
        lowGuid, itemGuidLow, static_cast<uint32>(socketIndex), stoneId);
}

static void DeleteSocketRow(uint32 itemGuidLow, uint8 socketIndex)
{
    // DirectExecute (SYNCHRONOUS): must commit before the following recompute's
    // sync read — see the note in InsertSocketRow.
    CharacterDatabase.DirectExecute(
        "DELETE FROM character_socketed_stones WHERE item_guid = {} AND socket_index = {}",
        itemGuidLow, static_cast<uint32>(socketIndex));
}

// Not called yet in Phase 3 (unsocket goes through DeleteSocketRow, keyed by
// item_guid+socketIndex, since that's what the .stone unsocket command has
// on hand) — kept as a ready-made helper for a future flow that only knows
// the stone id (e.g. "delete a stone entirely" cleanup).
[[maybe_unused]] static void DeleteSocketRowByStoneId(uint64 stoneId)
{
    // DirectExecute for the same reason as InsertSocketRow/DeleteSocketRow, so a
    // future caller can't reintroduce the async-write / sync-read race.
    CharacterDatabase.DirectExecute(
        "DELETE FROM character_socketed_stones WHERE stone_id = {}", stoneId);
}

// ---------------------------------------------------------------------------
// Stat application
// ---------------------------------------------------------------------------

// Per-player cache of the TOTAL amount currently applied to that player from
// socketed stones, so RecomputeStoneStats can cleanly undo the previous
// totals before applying the new ones — mirrors ApplyAAStat's add/remove
// pattern in mod-aa-system.cpp.
//
// Iron's armor bonus is applied as a FLAT armor value (armorFlat), NOT via
// ApplyStatPctModifier(UNIT_MOD_ARMOR, TOTAL_PCT). That percentage-multiply API
// has no apply/unapply bool and empirically produced an INVERTED result here
// (socketing a +14% armor stone LOWERED armor). Instead we compute a flat amount
// = pct% of the player's current (clean) armor and add it via
// HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, flat, apply) — the same
// explicit apply/unapply path used for Spirit. A positive flat add can only
// raise armor, and flat remove is exact, so this can never desync or invert.
// armorFlat caches the exact amount we added so the next recompute removes
// precisely that before recomputing from the clean armor value.
struct AppliedStoneStats
{
    int32 critRating     = 0;
    int32 hitMeleeRating = 0;
    int32 hitSpellRating = 0;
    int32 spirit         = 0;
    float armorFlat      = 0.0f; // flat armor we added (see note above)
    float amberPct       = 0.0f; // stored only — NOT applied in Phase 3
};

static std::unordered_map<uint32, AppliedStoneStats> g_appliedStoneStats;

// Recompute-from-scratch: undo whatever was previously applied for this
// player from socketed stones, recompute totals from the stones socketed
// into CURRENTLY EQUIPPED items, then reapply. Idempotent and safe to call
// redundantly (e.g. from both an equip hook and a socket command).
static void RecomputeStoneStats(Player* player)
{
    if (!player || !player->IsInWorld())
        return;

    uint32 lowGuid = player->GetGUID().GetCounter();

    // 1) UNDO previous totals, if any.
    auto prevIt = g_appliedStoneStats.find(lowGuid);
    if (prevIt != g_appliedStoneStats.end())
    {
        AppliedStoneStats const& prev = prevIt->second;
        player->ApplyRatingMod(CR_CRIT_MELEE,  prev.critRating, false);
        player->ApplyRatingMod(CR_CRIT_RANGED, prev.critRating, false);
        player->ApplyRatingMod(CR_CRIT_SPELL,  prev.critRating, false);
        player->ApplyRatingMod(CR_HIT_MELEE,   prev.hitMeleeRating, false);
        player->ApplyRatingMod(CR_HIT_RANGED,  prev.hitMeleeRating, false);
        player->ApplyRatingMod(CR_HIT_SPELL,   prev.hitSpellRating, false);
        player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT, TOTAL_VALUE, (float)prev.spirit, false);
        player->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, prev.armorFlat, false);
    }

    // 2) Build the set of currently-EQUIPPED item low-guids.
    std::unordered_set<uint32> equippedGuids;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            equippedGuids.insert(item->GetGUID().GetCounter());
    }

    // 3) Sum fresh totals from socketed stones on equipped items only.
    double critPct = 0.0, hitPct = 0.0, spiritFlat = 0.0, armorPct = 0.0, amberPct = 0.0;
    std::vector<SocketedStone> socketed = LoadSocketedStonesFromDB(lowGuid);
    for (SocketedStone const& s : socketed)
    {
        if (!equippedGuids.count(s.itemGuid))
            continue;

        char const* unit; double secondary; char const* secondaryUnit;
        double value = GetStoneStatValue(s.type, s.tier, s.rank, unit, secondary, secondaryUnit);
        (void)unit; (void)secondaryUnit; // only `value`/`secondary` are used below

        switch (s.type)
        {
            case STONE_CRIMSON:  critPct    += value; break;
            case STONE_OBSIDIAN: hitPct     += value; break;
            case STONE_JADE:     spiritFlat += value; break;
            case STONE_IRON:
                armorPct += value;
                // TODO Phase 4+: `secondary` (flat % phys DR at T4/T5) is
                // deliberately NOT applied here — no phys-DR hook exists yet.
                break;
            case STONE_AMBER:
                amberPct += value;
                // TODO Phase 4+: Amber's universal power (damage+healing %)
                // is deliberately NOT applied here — it needs the combat
                // damage/heal hooks, which are out of scope for Phase 3.
                break;
            default: break;
        }
    }

    // 4) Convert %->rating at level 80 (Sanctum's fixed level cap for this
    // conversion; see CLAUDE.md "Max level gate is 60" for AA but combat
    // rating conversion constants below are the standard WotLK L80 table —
    // matches the conversion Sanctum already uses elsewhere for AA rating).
    AppliedStoneStats fresh;
    fresh.critRating     = static_cast<int32>(std::lround(critPct * 45.9059));
    fresh.hitMeleeRating = static_cast<int32>(std::lround(hitPct * 32.79));
    fresh.hitSpellRating = static_cast<int32>(std::lround(hitPct * 26.232));
    fresh.spirit         = static_cast<int32>(std::lround(spiritFlat));
    fresh.amberPct       = static_cast<float>(amberPct);
    // fresh.armorFlat is computed at APPLY time below — it needs the clean
    // GetArmor() reading (after the undo step removed our previous flat).

    // 5) APPLY fresh totals.
    player->ApplyRatingMod(CR_CRIT_MELEE,  fresh.critRating, true);
    player->ApplyRatingMod(CR_CRIT_RANGED, fresh.critRating, true);
    player->ApplyRatingMod(CR_CRIT_SPELL,  fresh.critRating, true);
    player->ApplyRatingMod(CR_HIT_MELEE,   fresh.hitMeleeRating, true);
    player->ApplyRatingMod(CR_HIT_RANGED,  fresh.hitMeleeRating, true);
    player->ApplyRatingMod(CR_HIT_SPELL,   fresh.hitSpellRating, true);
    player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT, TOTAL_VALUE, (float)fresh.spirit, true);

    // Armor: convert the summed armor% into a FLAT armor add = pct% of the
    // player's CURRENT armor. GetArmor() here is "clean" — the undo step above
    // removed our previous flat, and the crit/hit/spirit applies don't touch
    // armor — so we're taking a percentage of the player's real base armor.
    if (armorPct > 0.0)
    {
        float cleanArmor = static_cast<float>(player->GetArmor());
        fresh.armorFlat  = cleanArmor * static_cast<float>(armorPct / 100.0);
        player->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, fresh.armorFlat, true);
    }
    player->UpdateAllStats();

    // 6) Store for the next undo pass.
    g_appliedStoneStats[lowGuid] = fresh;
}

// ---------------------------------------------------------------------------
// Migration hook (called from mod-gear-tiers.cpp when the Armory item tiers
// up and is destroyed+recreated with a new guid). Must stay at FILE SCOPE
// (non-static, external linkage) — mod-gear-tiers extern-declares it.
// ---------------------------------------------------------------------------
void PowerStones_OnItemMorphed(Player* player, uint32 oldItemGuidLow, Item* newItem)
{
    if (!newItem || oldItemGuidLow == 0)
        return;

    uint32 newGuid = newItem->GetGUID().GetCounter();

    std::vector<std::pair<uint8, uint64>> movedSockets = LoadSocketsForItem(oldItemGuidLow);
    if (movedSockets.empty())
        return;

    CharacterDatabase.DirectExecute(
        "UPDATE character_socketed_stones SET item_guid = {} WHERE item_guid = {}",
        newGuid, oldItemGuidLow);

    if (player)
    {
        RecomputeStoneStats(player);
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff9933ff[Power Stones]|r {} socketed stone(s) carried over to the upgraded item.",
            movedSockets.size());
    }
}

// ===========================================================================
// Player Script
// ===========================================================================

class PowerStonesPlayerScript : public PlayerScript
{
public:
    PowerStonesPlayerScript() : PlayerScript("PowerStonesPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        uint32 lowGuid = player->GetGUID().GetCounter();
        // Populate the cache now so buy/upgrade/list never hit the DB except
        // to write. Mirrors ConquestShardsPlayerScript::OnPlayerLogin.
        GetCachedStones(lowGuid);

        // PHASE 3 — orphan sweep: a socketed stone's item_guid can go stale
        // if the item was destroyed/sold/mailed away outside a tracked path
        // (or a DB edit). Drop any socket row whose item the player no
        // longer owns at all (equipped OR in bags — GetItemByGuid covers
        // both). The stone itself is untouched in character_power_stones —
        // it simply becomes unsocketed and stays in the collection.
        std::vector<SocketedStone> socketed = LoadSocketedStonesFromDB(lowGuid);
        for (SocketedStone const& s : socketed)
        {
            ObjectGuid itemGuid = ObjectGuid::Create<HighGuid::Item>(s.itemGuid);
            if (!player->GetItemByGuid(itemGuid))
                DeleteSocketRow(s.itemGuid, s.socketIndex);
        }

        RecomputeStoneStats(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        // Every buy/upgrade writes through to the DB immediately, so nothing
        // dirty needs flushing here. Drop the cache entry to bound memory use.
        uint32 lowGuid = player->GetGUID().GetCounter();
        g_ownedStones.erase(lowGuid);
        g_appliedStoneStats.erase(lowGuid);
    }

    // PHASE 3 — recompute on gear swap so socketing then changing gear
    // updates stats live (RecomputeStoneStats is idempotent/safe to call
    // redundantly). OnPlayerUnequip has no bag/slot/item-guid info beyond
    // the Item pointer, which is enough since RecomputeStoneStats rebuilds
    // the equipped-set from scratch each call.
    void OnPlayerEquip(Player* player, Item* /*it*/, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        RecomputeStoneStats(player);
    }

    void OnPlayerUnequip(Player* player, Item* /*it*/) override
    {
        RecomputeStoneStats(player);
    }
};

// ===========================================================================
// Command Script
// ===========================================================================

class PowerStonesCommandScript : public CommandScript
{
public:
    PowerStonesCommandScript() : CommandScript("PowerStonesCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable stoneTable =
        {
            { "buy",      HandleStoneBuy,      SEC_PLAYER, Console::Yes },
            { "upgrade",  HandleStoneUpgrade,  SEC_PLAYER, Console::Yes },
            { "list",     HandleStoneList,     SEC_PLAYER, Console::Yes },
            { "catalog",  HandleStoneCatalog,  SEC_PLAYER, Console::Yes },
            { "socket",   HandleStoneSocket,   SEC_PLAYER, Console::Yes },
            { "unsocket", HandleStoneUnsocket, SEC_PLAYER, Console::Yes },
            { "sockets",  HandleStoneSockets,  SEC_PLAYER, Console::Yes }
        };
        static ChatCommandTable commandTable =
        {
            { "stone", stoneTable }
        };
        return commandTable;
    }

    // Manual uint32 parse from a chat command arg — copied verbatim from
    // ConquestShardsCommandScript::ParseUInt32Arg.
    static bool ParseUInt32Arg(std::string_view argStr, uint32& outValue)
    {
        std::string s(argStr);
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        if (s.empty())
            return false;

        uint32 value = 0;
        for (char c : s)
        {
            if (c < '0' || c > '9')
                return false;
            value = value * 10 + static_cast<uint32>(c - '0');
        }
        outValue = value;
        return true;
    }

    static std::string TrimArg(std::string_view argStr)
    {
        std::string s(argStr);
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        return s;
    }

    // .stone buy <type>
    static bool HandleStoneBuy(ChatHandler* handler, std::string_view args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Power Stones]|r Must be used in-game.");
            return true;
        }

        std::string typeArg = TrimArg(args);
        uint8 type = ParseStoneTypeByName(typeArg);
        if (type == STONE_NONE)
        {
            handler->SendSysMessage(
                "|cffff0000[Power Stones]|r Usage: .stone buy <crimson|obsidian|jade|iron|amber>");
            return true;
        }

        if (!Shards_TrySpend(player, STONE_BUY_IN_COST, "stone-buy"))
        {
            int64 balance = Shards_GetBalance(player->GetGUID().GetCounter());
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r Not enough Conquest Shards (have {}, need {}).",
                balance, STONE_BUY_IN_COST);
            return true;
        }

        uint32 lowGuid = player->GetGUID().GetCounter();
        uint64 id = InsertOwnedStone(lowGuid, type, STONE_TIER_MIN, STONE_RANK_MIN);

        std::vector<OwnedStone>& stones = GetCachedStones(lowGuid);
        stones.push_back({ id, type, STONE_TIER_MIN, STONE_RANK_MIN });

        char const* unit; double secondary; char const* secondaryUnit;
        double value = GetStoneStatValue(type, STONE_TIER_MIN, STONE_RANK_MIN, unit, secondary, secondaryUnit);

        handler->PSendSysMessage(
            "|cff9933ff[Power Stones]|r Bought a new {} stone (#{}) — Tier I Rank 1 — {}{}.",
            StoneTypeName(type), id, FormatStoneValue(type, value), unit);
        return true;
    }

    // .stone upgrade <stoneId>
    static bool HandleStoneUpgrade(ChatHandler* handler, std::string_view args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Power Stones]|r Must be used in-game.");
            return true;
        }

        uint32 stoneId = 0;
        if (!ParseUInt32Arg(args, stoneId) || stoneId == 0)
        {
            handler->SendSysMessage("|cffff0000[Power Stones]|r Usage: .stone upgrade <stoneId>");
            return true;
        }

        uint32 lowGuid = player->GetGUID().GetCounter();
        std::vector<OwnedStone>& stones = GetCachedStones(lowGuid);

        OwnedStone* stone = nullptr;
        for (OwnedStone& s : stones)
        {
            if (s.id == stoneId)
            {
                stone = &s;
                break;
            }
        }

        if (!stone)
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r You don't own a stone with id #{}. Use .stone list.", stoneId);
            return true;
        }

        uint32 cost = 0;
        uint8 newTier = 0, newRank = 0;
        if (!GetNextUpgradeStep(stone->tier, stone->rank, cost, newTier, newRank))
        {
            handler->PSendSysMessage(
                "|cff9933ff[Power Stones]|r {} stone #{} is already Tier V Rank 3 — fully maxed.",
                StoneTypeName(stone->type), stone->id);
            return true;
        }

        if (!Shards_TrySpend(player, cost, "stone-upgrade"))
        {
            int64 balance = Shards_GetBalance(lowGuid);
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r Not enough Conquest Shards (have {}, need {}).",
                balance, cost);
            return true;
        }

        stone->tier = newTier;
        stone->rank = newRank;
        UpdateOwnedStone(stone->id, newTier, newRank);

        // If this stone is currently socketed into equipped gear, its new
        // (higher) value must be re-applied immediately. Without this, upgrading
        // a socketed stone changed only its DB row and the player's stats stayed
        // frozen at the value from when it was first socketed.
        RecomputeStoneStats(player);

        char const* unit; double secondary; char const* secondaryUnit;
        double value = GetStoneStatValue(stone->type, newTier, newRank, unit, secondary, secondaryUnit);

        std::string extra;
        if (secondary > 0.0 && *secondaryUnit)
            extra = " (+" + FormatStoneValue(STONE_NONE, secondary) + secondaryUnit + ")";

        handler->PSendSysMessage(
            "|cff9933ff[Power Stones]|r {} stone #{} upgraded to Tier {} Rank {} — {}{}{}. Spent {} shard(s).",
            StoneTypeName(stone->type), stone->id, static_cast<uint32>(newTier), static_cast<uint32>(newRank),
            FormatStoneValue(stone->type, value), unit, extra, cost);
        return true;
    }

    // .stone list
    static bool HandleStoneList(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Power Stones]|r Must be used in-game.");
            return true;
        }

        uint32 lowGuid = player->GetGUID().GetCounter();
        std::vector<OwnedStone>& stones = GetCachedStones(lowGuid);

        if (stones.empty())
        {
            handler->SendSysMessage(
                "|cff9933ff[Power Stones]|r You don't own any stones yet. Try .stone buy <type>.");
            return true;
        }

        // PHASE 3: tag socketed stones so the player can tell which of their
        // stones are currently in use.
        std::vector<SocketedStone> socketed = LoadSocketedStonesFromDB(lowGuid);

        handler->SendSysMessage("|cff9933ff[Power Stones] Your Collection|r");
        for (OwnedStone const& s : stones)
        {
            char const* unit; double secondary; char const* secondaryUnit;
            double value = GetStoneStatValue(s.type, s.tier, s.rank, unit, secondary, secondaryUnit);

            std::string extra;
            if (secondary > 0.0 && *secondaryUnit)
                extra = " (+" + FormatStoneValue(STONE_NONE, secondary) + secondaryUnit + ")";

            std::string socketTag;
            for (SocketedStone const& sock : socketed)
            {
                if (sock.stoneId == s.id)
                {
                    socketTag = "  |cff888888[socketed]|r";
                    break;
                }
            }

            handler->PSendSysMessage("  #{} {} T{} R{} — {}{}{}{}",
                s.id, StoneTypeName(s.type), static_cast<uint32>(s.tier), static_cast<uint32>(s.rank),
                FormatStoneValue(s.type, value), unit, extra, socketTag);
        }
        return true;
    }

    // .stone catalog — static reference menu, works from console too.
    static bool HandleStoneCatalog(ChatHandler* handler)
    {
        handler->SendSysMessage("|cff9933ff[Power Stones] Catalog|r (buy-in: "
            + std::to_string(STONE_BUY_IN_COST) + " shards)");

        for (uint8 type = STONE_TYPE_MIN; type <= STONE_TYPE_MAX; ++type)
        {
            char const* unitMin; double secMin; char const* secUnitMin;
            char const* unitMax; double secMax; char const* secUnitMax;
            double valMin = GetStoneStatValue(type, STONE_TIER_MIN, STONE_RANK_MIN, unitMin, secMin, secUnitMin);
            double valMax = GetStoneStatValue(type, STONE_TIER_MAX, STONE_RANK_MAX, unitMax, secMax, secUnitMax);

            handler->PSendSysMessage("  {} — Tier I R1: {}{}  ->  Tier V R3: {}{}",
                StoneTypeName(type), FormatStoneValue(type, valMin), unitMin,
                FormatStoneValue(type, valMax), unitMax);
        }

        handler->SendSysMessage(
            "|cff9933ff[Power Stones]|r Note: Void (lifesteal) stone is deferred, not yet buyable.");
        return true;
    }

    // .stone socket <stoneId> <slotName> <socketIndex>
    // NOTE: args is Acore::ChatCommands::Tail (not std::string_view). A plain
    // std::string_view parameter binds only the FIRST whitespace-delimited token,
    // so a 3-token command like ".stone socket 2 chest 1" left "chest 1"
    // unconsumed and the framework rejected the whole command with a USAGE fault.
    // Tail captures the entire remaining line (it derives from std::string_view,
    // so the manual tokenizer below is unchanged). Core multi-word commands
    // (.go, .lookup, …) use Tail for exactly this reason.
    static bool HandleStoneSocket(ChatHandler* handler, Tail args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Power Stones]|r Must be used in-game.");
            return true;
        }

        // Manual 3-token split (stoneId, slotName, socketIndex) — args can
        // contain spaces only within nothing here (single-word tokens), so a
        // simple whitespace split is sufficient.
        std::string argsStr(args);
        std::vector<std::string> tokens;
        {
            std::string cur;
            for (char c : argsStr)
            {
                if (c == ' ' || c == '\t')
                {
                    if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                }
                else
                    cur += c;
            }
            if (!cur.empty())
                tokens.push_back(cur);
        }

        if (tokens.size() != 3)
        {
            handler->SendSysMessage(
                "|cffff0000[Power Stones]|r Usage: .stone socket <stoneId> <slotName> <socketIndex>");
            return true;
        }

        uint32 stoneId = 0;
        uint32 socketIndexArg = 0;
        if (!ParseUInt32Arg(tokens[0], stoneId) || stoneId == 0 || !ParseUInt32Arg(tokens[2], socketIndexArg) || socketIndexArg == 0)
        {
            handler->SendSysMessage(
                "|cffff0000[Power Stones]|r Usage: .stone socket <stoneId> <slotName> <socketIndex>");
            return true;
        }
        uint8 socketIndex = static_cast<uint8>(socketIndexArg);

        int slot = ParseEquipSlotByName(tokens[1]);
        if (slot < 0)
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r Unknown slot name '{}'. Try: head, neck, shoulders, back, chest, "
                "wrists, hands, waist, legs, feet, finger1, finger2, trinket1, trinket2, mainhand, offhand, ranged.",
                tokens[1]);
            return true;
        }

        uint32 lowGuid = player->GetGUID().GetCounter();

        // Ownership check.
        std::vector<OwnedStone>& stones = GetCachedStones(lowGuid);
        OwnedStone* stone = nullptr;
        for (OwnedStone& s : stones)
        {
            if (s.id == stoneId) { stone = &s; break; }
        }
        if (!stone)
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r You don't own a stone with id #{}. Use .stone list.", stoneId);
            return true;
        }

        // Not already socketed anywhere.
        if (IsStoneSocketed(lowGuid, stoneId))
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r Stone #{} is already socketed. Use .stone unsocket first.", stoneId);
            return true;
        }

        // Item must be equipped in that slot.
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, static_cast<uint8>(slot));
        if (!item)
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r You have no item equipped in slot '{}'.", tokens[1]);
            return true;
        }

        // Socket index must be in range for that item's quality-derived socket count.
        uint8 maxSockets = SocketCountForItem(item);
        if (socketIndex < 1 || socketIndex > maxSockets)
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r {} only has {} socket(s) (quality-based). Valid range: 1-{}.",
                item->GetTemplate()->Name1, static_cast<uint32>(maxSockets), static_cast<uint32>(maxSockets));
            return true;
        }

        // Socket must be empty.
        uint32 itemGuidLow = item->GetGUID().GetCounter();
        std::vector<std::pair<uint8, uint64>> existingSockets = LoadSocketsForItem(itemGuidLow);
        for (auto const& [idx, sId] : existingSockets)
        {
            if (idx == socketIndex)
            {
                handler->PSendSysMessage(
                    "|cffff0000[Power Stones]|r Socket {} on {} is already occupied (stone #{}). Unsocket it first.",
                    static_cast<uint32>(socketIndex), item->GetTemplate()->Name1, sId);
                return true;
            }
        }

        InsertSocketRow(lowGuid, itemGuidLow, socketIndex, stoneId);
        RecomputeStoneStats(player);

        char const* unit; double secondary; char const* secondaryUnit;
        double value = GetStoneStatValue(stone->type, stone->tier, stone->rank, unit, secondary, secondaryUnit);

        handler->PSendSysMessage(
            "|cff9933ff[Power Stones]|r Socketed {} stone #{} ({}{}) into {} — {} socket {}. Stats updated.",
            StoneTypeName(stone->type), stone->id, FormatStoneValue(stone->type, value), unit,
            item->GetTemplate()->Name1, tokens[1], static_cast<uint32>(socketIndex));
        return true;
    }

    // .stone unsocket <slotName> <socketIndex> — Tail (2 tokens); see the note
    // on HandleStoneSocket for why this must not be a plain std::string_view.
    static bool HandleStoneUnsocket(ChatHandler* handler, Tail args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Power Stones]|r Must be used in-game.");
            return true;
        }

        std::string argsStr(args);
        std::vector<std::string> tokens;
        {
            std::string cur;
            for (char c : argsStr)
            {
                if (c == ' ' || c == '\t')
                {
                    if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                }
                else
                    cur += c;
            }
            if (!cur.empty())
                tokens.push_back(cur);
        }

        if (tokens.size() != 2)
        {
            handler->SendSysMessage(
                "|cffff0000[Power Stones]|r Usage: .stone unsocket <slotName> <socketIndex>");
            return true;
        }

        int slot = ParseEquipSlotByName(tokens[0]);
        uint32 socketIndexArg = 0;
        if (slot < 0 || !ParseUInt32Arg(tokens[1], socketIndexArg) || socketIndexArg == 0)
        {
            handler->SendSysMessage(
                "|cffff0000[Power Stones]|r Usage: .stone unsocket <slotName> <socketIndex>");
            return true;
        }
        uint8 socketIndex = static_cast<uint8>(socketIndexArg);

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, static_cast<uint8>(slot));
        if (!item)
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r You have no item equipped in slot '{}'.", tokens[0]);
            return true;
        }

        uint32 itemGuidLow = item->GetGUID().GetCounter();
        std::vector<std::pair<uint8, uint64>> existingSockets = LoadSocketsForItem(itemGuidLow);
        uint64 stoneId = 0;
        for (auto const& [idx, sId] : existingSockets)
        {
            if (idx == socketIndex) { stoneId = sId; break; }
        }

        if (stoneId == 0)
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r Socket {} on {} is already empty.",
                static_cast<uint32>(socketIndex), item->GetTemplate()->Name1);
            return true;
        }

        // PHASE 4 — tiered GOLD swap fee (GDD 5g-250g by tier). Look up the
        // stone's tier from the owned-stone cache to price the fee. If the stone
        // isn't in the cache (shouldn't happen — it's socketed, so it's owned),
        // tier 0 -> fee 0, i.e. fail-open to a free unsocket rather than block it.
        uint32 lowGuid = player->GetGUID().GetCounter();
        uint8 stoneTier = 0;
        for (OwnedStone const& os : GetCachedStones(lowGuid))
        {
            if (os.id == stoneId) { stoneTier = os.tier; break; }
        }

        uint32 feeCopper = StoneUnsocketGoldCost(stoneTier);
        if (feeCopper > 0 && !player->HasEnoughMoney(static_cast<int32>(feeCopper)))
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r Removing a Tier {} stone costs {}g. You don't have enough gold.",
                static_cast<uint32>(stoneTier), feeCopper / 10000);
            return true;
        }

        // Charge first, THEN delete — so an unaffordable swap can never strip the
        // socket for free (the affordability guard above already returned).
        if (feeCopper > 0)
            player->ModifyMoney(-static_cast<int32>(feeCopper));

        DeleteSocketRow(itemGuidLow, socketIndex);
        RecomputeStoneStats(player);

        if (feeCopper > 0)
            handler->PSendSysMessage(
                "|cff9933ff[Power Stones]|r Stone #{} removed from {} socket {} for {}g and returned to your collection.",
                stoneId, item->GetTemplate()->Name1, static_cast<uint32>(socketIndex), feeCopper / 10000);
        else
            handler->PSendSysMessage(
                "|cff9933ff[Power Stones]|r Stone #{} removed from {} socket {} and returned to your collection.",
                stoneId, item->GetTemplate()->Name1, static_cast<uint32>(socketIndex));
        return true;
    }

    // .stone sockets <slotName>
    static bool HandleStoneSockets(ChatHandler* handler, std::string_view args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Power Stones]|r Must be used in-game.");
            return true;
        }

        std::string slotArg = TrimArg(args);
        int slot = ParseEquipSlotByName(slotArg);
        if (slot < 0)
        {
            handler->SendSysMessage("|cffff0000[Power Stones]|r Usage: .stone sockets <slotName>");
            return true;
        }

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, static_cast<uint8>(slot));
        if (!item)
        {
            handler->PSendSysMessage(
                "|cffff0000[Power Stones]|r You have no item equipped in slot '{}'.", slotArg);
            return true;
        }

        uint8 maxSockets = SocketCountForItem(item);
        uint32 itemGuidLow = item->GetGUID().GetCounter();
        std::vector<std::pair<uint8, uint64>> existingSockets = LoadSocketsForItem(itemGuidLow);

        handler->PSendSysMessage("|cff9933ff[Power Stones]|r {} — {} socket(s):",
            item->GetTemplate()->Name1, static_cast<uint32>(maxSockets));

        for (uint8 idx = 1; idx <= maxSockets; ++idx)
        {
            uint64 stoneId = 0;
            for (auto const& [sIdx, sId] : existingSockets)
            {
                if (sIdx == idx) { stoneId = sId; break; }
            }

            if (stoneId == 0)
            {
                handler->PSendSysMessage("  Socket {}: |cff888888empty|r", static_cast<uint32>(idx));
                continue;
            }

            // Look up type/tier/rank for display.
            QueryResult result = CharacterDatabase.Query(
                "SELECT stone_type, tier, `rank` FROM character_power_stones WHERE id = {}", stoneId);
            if (!result)
            {
                handler->PSendSysMessage("  Socket {}: stone #{} (data missing)", static_cast<uint32>(idx), stoneId);
                continue;
            }

            Field* fields = result->Fetch();
            uint8 type = fields[0].Get<uint8>();
            uint8 tier = fields[1].Get<uint8>();
            uint8 rank = fields[2].Get<uint8>();

            char const* unit; double secondary; char const* secondaryUnit;
            double value = GetStoneStatValue(type, tier, rank, unit, secondary, secondaryUnit);

            handler->PSendSysMessage("  Socket {}: #{} {} T{} R{} — {}{}",
                static_cast<uint32>(idx), stoneId, StoneTypeName(type),
                static_cast<uint32>(tier), static_cast<uint32>(rank),
                FormatStoneValue(type, value), unit);
        }
        return true;
    }
};

// ===========================================================================
// World Script
// ===========================================================================

class PowerStonesWorldScript : public WorldScript
{
public:
    PowerStonesWorldScript() : WorldScript("PowerStonesWorldScript") {}

    void OnStartup() override
    {
        LOG_INFO("module", "[mod-power-stones] Module loaded.");
    }
};

// ===========================================================================
// PHASE 4 — Acquisition NPC (gossip shop front)
// ===========================================================================
//
// "Lapidary Voss, Power Stone Broker" (creature_template entry 700260, script
// npc_power_stone_broker). A pure SHOP: buy new stones, rank/tier up owned
// stones, and view your collection — all spending Conquest Shards, exactly
// mirroring the .stone buy / .stone upgrade / .stone list commands so the two
// paths stay in lockstep. Socketing is intentionally NOT here (it stays on the
// .stone socket commands + the Phase 6 Lua panel — decision locked 2026-07-15).
//
// Same single-CreatureScript pattern as mod-sanctum-attendant; reskin/rename/
// relocate purely via the module SQL (name + CreatureDisplayID + spawn row),
// no code change. Lives in THIS translation unit so it reuses every catalog
// helper (GetStoneStatValue / GetNextUpgradeStep / GetCachedStones / Insert-
// OwnedStone / RecomputeStoneStats) and the shards API with no header plumbing.

static const uint32 BROKER_NPC_ENTRY   = 700260;
static const uint32 BROKER_NPC_TEXT     = 700260; // created in the module SQL

// Main-menu actions (sender == GOSSIP_SENDER_MAIN).
enum BrokerAction
{
    BACT_BUY_MENU     = 1,
    BACT_UPGRADE_MENU = 2,
    BACT_COLLECTION   = 3,
    BACT_BACK         = 8,
    BACT_CLOSE        = 9
};

// Custom gossip senders that repurpose `action` as a data field:
//   SENDER_BUY     -> action carries the stone TYPE (1..5)
//   SENDER_UPGRADE -> action carries the owned stone ID (fits uint32 on a solo
//                     server; AUTO_INCREMENT starts at 1)
static const uint32 SENDER_BUY     = 1001;
static const uint32 SENDER_UPGRADE = 1002;

class npc_power_stone_broker : public CreatureScript
{
public:
    npc_power_stone_broker() : CreatureScript("npc_power_stone_broker") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ShowMainMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        // Buy a specific stone type.
        if (sender == SENDER_BUY)
        {
            DoBuy(player, static_cast<uint8>(action));
            ShowBuyMenu(player, creature); // stay in the buy menu for more purchases
            return true;
        }

        // Upgrade a specific owned stone (action == stoneId).
        if (sender == SENDER_UPGRADE)
        {
            DoUpgrade(player, action);
            ShowUpgradeMenu(player, creature); // refresh — new tier/rank + next cost
            return true;
        }

        switch (action)
        {
            case BACT_BUY_MENU:     ShowBuyMenu(player, creature);     break;
            case BACT_UPGRADE_MENU: ShowUpgradeMenu(player, creature); break;
            case BACT_COLLECTION:   ShowCollection(player, creature);  break;
            case BACT_BACK:         ShowMainMenu(player, creature);    break;
            case BACT_CLOSE:
            default:                CloseGossipMenuFor(player);        break;
        }
        return true;
    }

private:
    // ---- menus ------------------------------------------------------------

    void ShowMainMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        int64 balance = Shards_GetBalance(player->GetGUID().GetCounter());

        AddGossipItemFor(player, GOSSIP_ICON_VENDOR,
            "Buy a Power Stone.", GOSSIP_SENDER_MAIN, BACT_BUY_MENU);
        AddGossipItemFor(player, GOSSIP_ICON_TALK,
            "Upgrade one of my stones.", GOSSIP_SENDER_MAIN, BACT_UPGRADE_MENU);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "Show me my collection.", GOSSIP_SENDER_MAIN, BACT_COLLECTION);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "Farewell.", GOSSIP_SENDER_MAIN, BACT_CLOSE);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff9933ff[Power Stones]|r You carry {} Conquest Shard(s).", balance);
        SendGossipMenuFor(player, BROKER_NPC_TEXT, creature->GetGUID());
    }

    void ShowBuyMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);

        for (uint8 type = STONE_TYPE_MIN; type <= STONE_TYPE_MAX; ++type)
        {
            char const* unit; double secondary; char const* secondaryUnit;
            double value = GetStoneStatValue(type, STONE_TIER_MIN, STONE_RANK_MIN, unit, secondary, secondaryUnit);

            std::string label = std::string(StoneTypeName(type)) + " — Tier I Rank 1 ("
                + FormatStoneValue(type, value) + unit + ") — "
                + std::to_string(STONE_BUY_IN_COST) + " shards";

            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, label, SENDER_BUY, type);
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", GOSSIP_SENDER_MAIN, BACT_BACK);
        SendGossipMenuFor(player, BROKER_NPC_TEXT, creature->GetGUID());
    }

    void ShowUpgradeMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        uint32 lowGuid = player->GetGUID().GetCounter();
        std::vector<OwnedStone>& stones = GetCachedStones(lowGuid);

        uint32 shown = 0;
        for (OwnedStone const& s : stones)
        {
            uint32 cost = 0; uint8 newTier = 0, newRank = 0;
            if (!GetNextUpgradeStep(s.tier, s.rank, cost, newTier, newRank))
                continue; // already maxed — not upgradable

            std::string label = "#" + std::to_string(s.id) + " " + StoneTypeName(s.type)
                + " T" + std::to_string(s.tier) + " R" + std::to_string(s.rank)
                + " -> T" + std::to_string(newTier) + " R" + std::to_string(newRank)
                + " (" + std::to_string(cost) + " shards)";

            // action carries the stone id; sender routes it to DoUpgrade.
            AddGossipItemFor(player, GOSSIP_ICON_TALK, label, SENDER_UPGRADE, static_cast<uint32>(s.id));
            ++shown;
        }

        if (shown == 0)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff9933ff[Power Stones]|r You have no upgradable stones (buy one, or all are maxed).");

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", GOSSIP_SENDER_MAIN, BACT_BACK);
        SendGossipMenuFor(player, BROKER_NPC_TEXT, creature->GetGUID());
    }

    void ShowCollection(Player* player, Creature* creature)
    {
        uint32 lowGuid = player->GetGUID().GetCounter();
        std::vector<OwnedStone>& stones = GetCachedStones(lowGuid);
        ChatHandler ch(player->GetSession());

        if (stones.empty())
            ch.PSendSysMessage("|cff9933ff[Power Stones]|r You own no stones yet. Buy one to begin.");
        else
        {
            ch.PSendSysMessage("|cff9933ff[Power Stones] Your Collection|r");
            for (OwnedStone const& s : stones)
            {
                char const* unit; double secondary; char const* secondaryUnit;
                double value = GetStoneStatValue(s.type, s.tier, s.rank, unit, secondary, secondaryUnit);
                ch.PSendSysMessage("  #{} {} T{} R{} — {}{}",
                    s.id, StoneTypeName(s.type), static_cast<uint32>(s.tier), static_cast<uint32>(s.rank),
                    FormatStoneValue(s.type, value), unit);
            }
        }

        // Re-show the main menu so the window stays open.
        ShowMainMenu(player, creature);
    }

    // ---- actions (mirror the .stone buy / .stone upgrade command logic) ----

    static void DoBuy(Player* player, uint8 type)
    {
        ChatHandler ch(player->GetSession());
        if (type < STONE_TYPE_MIN || type > STONE_TYPE_MAX)
            return;

        if (!Shards_TrySpend(player, STONE_BUY_IN_COST, "stone-buy-npc"))
        {
            int64 balance = Shards_GetBalance(player->GetGUID().GetCounter());
            ch.PSendSysMessage("|cffff0000[Power Stones]|r Not enough Conquest Shards (have {}, need {}).",
                balance, STONE_BUY_IN_COST);
            return;
        }

        uint32 lowGuid = player->GetGUID().GetCounter();
        uint64 id = InsertOwnedStone(lowGuid, type, STONE_TIER_MIN, STONE_RANK_MIN);
        GetCachedStones(lowGuid).push_back({ id, type, STONE_TIER_MIN, STONE_RANK_MIN });

        char const* unit; double secondary; char const* secondaryUnit;
        double value = GetStoneStatValue(type, STONE_TIER_MIN, STONE_RANK_MIN, unit, secondary, secondaryUnit);
        ch.PSendSysMessage(
            "|cff9933ff[Power Stones]|r Bought a new {} stone (#{}) — Tier I Rank 1 — {}{}.",
            StoneTypeName(type), id, FormatStoneValue(type, value), unit);
    }

    static void DoUpgrade(Player* player, uint32 stoneId)
    {
        ChatHandler ch(player->GetSession());
        uint32 lowGuid = player->GetGUID().GetCounter();
        std::vector<OwnedStone>& stones = GetCachedStones(lowGuid);

        OwnedStone* stone = nullptr;
        for (OwnedStone& s : stones)
            if (s.id == stoneId) { stone = &s; break; }
        if (!stone)
            return; // stale menu click — stone no longer owned

        uint32 cost = 0; uint8 newTier = 0, newRank = 0;
        if (!GetNextUpgradeStep(stone->tier, stone->rank, cost, newTier, newRank))
        {
            ch.PSendSysMessage("|cff9933ff[Power Stones]|r {} stone #{} is already fully maxed.",
                StoneTypeName(stone->type), stone->id);
            return;
        }

        if (!Shards_TrySpend(player, cost, "stone-upgrade-npc"))
        {
            int64 balance = Shards_GetBalance(lowGuid);
            ch.PSendSysMessage("|cffff0000[Power Stones]|r Not enough Conquest Shards (have {}, need {}).",
                balance, cost);
            return;
        }

        stone->tier = newTier;
        stone->rank = newRank;
        UpdateOwnedStone(stone->id, newTier, newRank);
        // Re-apply live if the stone is socketed into equipped gear (mirrors the
        // command path — otherwise an upgraded socketed stone's stats stay frozen).
        RecomputeStoneStats(player);

        char const* unit; double secondary; char const* secondaryUnit;
        double value = GetStoneStatValue(stone->type, newTier, newRank, unit, secondary, secondaryUnit);
        ch.PSendSysMessage(
            "|cff9933ff[Power Stones]|r {} stone #{} upgraded to Tier {} Rank {} — {}{}. Spent {} shard(s).",
            StoneTypeName(stone->type), stone->id, static_cast<uint32>(newTier), static_cast<uint32>(newRank),
            FormatStoneValue(stone->type, value), unit, cost);
    }
};

// ===========================================================================
// Registration
// ===========================================================================

void AddSC_mod_power_stones()
{
    new PowerStonesPlayerScript();
    new PowerStonesCommandScript();
    new PowerStonesWorldScript();
    new npc_power_stone_broker();
}
