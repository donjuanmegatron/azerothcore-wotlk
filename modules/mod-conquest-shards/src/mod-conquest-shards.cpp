// mod-conquest-shards.cpp
//
// Sanctum Conquest Shards — server-wide alt-currency wallet.
// -----------------------------------------------------------
// Earned from boss kills (dungeon/raid), spent on future shard shops
// (power stones, inscriptions, PvP recipes, gems, GXP boosts...).
// See memory/project_conquest_shard.md for the full design.
//
// This module owns:
//   - character_conquest_shards  (current balance per character)
//   - conquest_shard_ledger     (audit trail of every award/spend)
//
// Public Spend API (sanctum_shards.h) is consumed by future modules the same
// way mod-aa-system consumes GearTiers_AddGXP from mod-gear-tiers.
//
// Award sources (current):
//   Dungeon boss (non-respawn only, gated OFF for now) = 1 shard
//   Dungeon final boss (TODO, not yet detected)          = 3 shards
//   Raid boss                                            = 6 shards
//   Raid final boss (TODO, not yet detected)              = 12 shards
//
// GM/testing commands: .shards balance|award|buygxp|log (see command table).

#include "sanctum_shards.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "CommandScript.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "Map.h"
#include <unordered_map>
#include <string>

using namespace Acore::ChatCommands;

// ---------------------------------------------------------------------------
// Extern from mod-gear-tiers — proves the wallet->spend->effect pipeline
// end-to-end via ".shards buygxp". Declared here rather than editing
// mod-gear-tiers.cpp, matching the pattern mod-aa-system already uses.
// ---------------------------------------------------------------------------
extern void GearTiers_AddGXP(Player* player, uint32 amount);

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint32 SHARD_DUNGEON_BOSS  = 1;
static constexpr uint32 SHARD_DUNGEON_FINAL = 3;
static constexpr uint32 SHARD_RAID_BOSS     = 6;
static constexpr uint32 SHARD_RAID_FINAL    = 12;

// GXP granted per shard spent via ".shards buygxp" (proof-of-life spend sink).
static constexpr uint32 GXP_PER_SHARD = 200;

// ---------------------------------------------------------------------------
// In-memory balance cache, keyed by low-part GUID. Loaded lazily on login.
// ---------------------------------------------------------------------------

static std::unordered_map<uint32, int64> g_shardBalance;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Ensures the cache has an entry for this GUID, loading from the DB if needed.
// Synchronous — matches the login-time query pattern used by mod-gear-tiers'
// ArmorySlotPlayerScript::OnPlayerLogin (blocking DB call at login is fine here).
static int64 LoadBalanceFromDB(uint32 lowGuid)
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT balance FROM character_conquest_shards WHERE guid = {}", lowGuid);
    if (!result)
        return 0;

    Field* fields = result->Fetch();
    return static_cast<int64>(fields[0].Get<uint64>());
}

static int64& GetCachedBalance(uint32 lowGuid)
{
    auto it = g_shardBalance.find(lowGuid);
    if (it != g_shardBalance.end())
        return it->second;

    int64 loaded = LoadBalanceFromDB(lowGuid);
    return g_shardBalance.emplace(lowGuid, loaded).first->second;
}

static void SaveBalance(uint32 lowGuid, int64 balance)
{
    CharacterDatabase.Execute(
        "REPLACE INTO character_conquest_shards (guid, balance) VALUES ({}, {})",
        lowGuid, static_cast<uint64>(balance));
}

// `reason` is always a fixed internal string literal passed by our own call
// sites (e.g. "raid-boss", "gm", "gxp-boost") — never built from player input —
// so no escaping is required. If a future call site ever derives `reason` from
// player-supplied text, route it through CharacterDatabase.EscapeString first.
static void WriteLedger(uint32 lowGuid, int64 delta, char const* reason)
{
    CharacterDatabase.Execute(
        "INSERT INTO conquest_shard_ledger (guid, delta, reason) VALUES ({}, {}, '{}')",
        lowGuid, delta, reason);
}

static void SendShardMessage(Player* player, int64 delta, char const* reason, int64 newBalance)
{
    if (!player || !player->GetSession())
        return;

    if (delta >= 0)
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff9933ff[Conquest Shards]|r +{} ({}). Balance: {}",
            delta, reason, newBalance);
    }
    else
    {
        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff9933ff[Conquest Shards]|r {} ({}). Balance: {}",
            delta, reason, newBalance);
    }
}

// ---------------------------------------------------------------------------
// Public API (sanctum_shards.h)
// ---------------------------------------------------------------------------

int64 Shards_GetBalance(uint32 lowGuid)
{
    auto it = g_shardBalance.find(lowGuid);
    if (it != g_shardBalance.end())
        return it->second;

    // Not cached (e.g. queried before this character ever logged in this
    // session) — fall back to a direct DB read without populating the cache.
    return LoadBalanceFromDB(lowGuid);
}

bool Shards_TrySpend(Player* player, uint32 amount, char const* reason)
{
    if (!player || amount == 0)
        return false;

    uint32 lowGuid   = player->GetGUID().GetCounter();
    int64& balance    = GetCachedBalance(lowGuid);

    if (balance < static_cast<int64>(amount))
        return false;

    balance -= static_cast<int64>(amount);
    SaveBalance(lowGuid, balance);
    WriteLedger(lowGuid, -static_cast<int64>(amount), reason);
    SendShardMessage(player, -static_cast<int64>(amount), reason, balance);
    return true;
}

void Shards_Award(Player* player, uint32 amount, char const* reason)
{
    if (!player || amount == 0)
        return;

    uint32 lowGuid = player->GetGUID().GetCounter();
    int64& balance  = GetCachedBalance(lowGuid);

    balance += static_cast<int64>(amount);
    SaveBalance(lowGuid, balance);
    WriteLedger(lowGuid, static_cast<int64>(amount), reason);
    SendShardMessage(player, static_cast<int64>(amount), reason, balance);
}

// ---------------------------------------------------------------------------
// Kill hook helpers
// ---------------------------------------------------------------------------

// TODO: wire to mod-zone-instances ZoneInstances_IsNonRespawn once that module
// exposes a public query. Until then dungeon shard drops are coded but OFF.
static bool ShardsDungeonNonRespawn(Map* /*map*/)
{
    return false;
}

// ===========================================================================
// Player Script
// ===========================================================================

class ConquestShardsPlayerScript : public PlayerScript
{
public:
    ConquestShardsPlayerScript() : PlayerScript("ConquestShardsPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        uint32 lowGuid = player->GetGUID().GetCounter();
        // Populate the cache now so later reads/spends in this session never
        // hit the DB except to save. Mirrors ArmorySlotPlayerScript::OnPlayerLogin.
        GetCachedBalance(lowGuid);
    }

    void OnPlayerLogout(Player* player) override
    {
        // Balance is saved on every change (Award/TrySpend), so nothing dirty
        // needs flushing at logout. Drop the cache entry to bound memory use.
        uint32 lowGuid = player->GetGUID().GetCounter();
        g_shardBalance.erase(lowGuid);
    }

    // Exact override name/signature copied from mod-gear-tiers.cpp
    // ArmorySlotPlayerScript::OnPlayerCreatureKill.
    void OnPlayerCreatureKill(Player* player, Creature* killed) override
    {
        if (!killed || killed->IsPet() || killed->IsTotem() || killed->IsCritter())
            return;

        CreatureTemplate const* cInfo = killed->GetCreatureTemplate();
        if (!cInfo)
            return;

        // Only WorldBoss-ranked creatures count as a "boss" kill for shards.
        if (cInfo->rank != CREATURE_ELITE_WORLDBOSS)
            return;

        Map* map = player->GetMap();
        if (!map)
            return;

        if (map->IsRaid())
        {
            // TODO: raid final-boss detection -> SHARD_RAID_FINAL
            Shards_Award(player, SHARD_RAID_BOSS, "raid-boss");
        }
        else if (map->IsDungeon() && !map->IsRaid())
        {
            if (ShardsDungeonNonRespawn(map))
                Shards_Award(player, SHARD_DUNGEON_BOSS, "dungeon-boss");
        }
    }
};

// ===========================================================================
// Command Script
// ===========================================================================

class ConquestShardsCommandScript : public CommandScript
{
public:
    ConquestShardsCommandScript() : CommandScript("ConquestShardsCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable shardsTable =
        {
            { "balance", HandleShardsBalance, SEC_PLAYER,     Console::Yes },
            { "award",   HandleShardsAward,   SEC_GAMEMASTER, Console::Yes },
            { "buygxp",  HandleShardsBuyGxp,  SEC_PLAYER,     Console::Yes },
            { "log",     HandleShardsLog,     SEC_PLAYER,     Console::Yes }
        };
        static ChatCommandTable commandTable =
        {
            { "shards", shardsTable }
        };
        return commandTable;
    }

    // Manual uint32 parse from a chat command arg — matches the pattern used
    // throughout mod-gear-tiers/mod-aa-system ("AzerothCore uint32 arg parser
    // quirks" — see HandleSetEntry / HandleAaAddPointsCommand). Returns false
    // (leaving outValue untouched) if the trimmed arg is empty or non-numeric.
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

    // .shards balance [character name]
    // With no arg: selected player or self (in-game). From SOAP/console with
    // no selected unit, a name arg is required. Mirrors .aa testall's
    // FindPlayerByName-with-fallback pattern.
    static bool HandleShardsBalance(ChatHandler* handler, std::string_view args)
    {
        Player* player = nullptr;
        std::string name(args);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r' || name.back() == '\n'))
            name.pop_back();

        if (!name.empty())
        {
            player = ObjectAccessor::FindPlayerByName(name, true);
            if (!player)
            {
                handler->PSendSysMessage("|cffff0000[Conquest Shards]|r Player '{}' is not online.", name);
                return true;
            }
        }
        else
        {
            player = handler->getSelectedPlayerOrSelf();
        }

        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Conquest Shards]|r No valid target. Usage: .shards balance [character name]");
            return true;
        }

        uint32 lowGuid = player->GetGUID().GetCounter();
        int64 balance = Shards_GetBalance(lowGuid);

        handler->PSendSysMessage(
            "|cff9933ff[Conquest Shards]|r {}'s balance: {}", player->GetName(), balance);
        return true;
    }

    // .shards award <amount> — GM: award to selected player/self.
    static bool HandleShardsAward(ChatHandler* handler, std::string_view args)
    {
        uint32 amount = 0;
        if (!ParseUInt32Arg(args, amount) || amount == 0)
        {
            handler->SendSysMessage("|cffff0000[Conquest Shards]|r Usage: .shards award <amount>");
            return true;
        }

        Player* player = handler->getSelectedPlayerOrSelf();
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Conquest Shards]|r No valid target.");
            return true;
        }

        Shards_Award(player, amount, "gm");
        handler->PSendSysMessage(
            "|cff9933ff[Conquest Shards]|r Awarded {} shard(s) to {}.", amount, player->GetName());
        return true;
    }

    // .shards buygxp <amount> — proof-of-life store item: spend shards, grant
    // Gear XP to the player's Armory Slot item via mod-gear-tiers' public API.
    static bool HandleShardsBuyGxp(ChatHandler* handler, std::string_view args)
    {
        uint32 amount = 0;
        if (!ParseUInt32Arg(args, amount) || amount == 0)
        {
            handler->SendSysMessage("|cffff0000[Conquest Shards]|r Usage: .shards buygxp <amount>");
            return true;
        }

        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Conquest Shards]|r Must be used in-game.");
            return true;
        }

        if (!Shards_TrySpend(player, amount, "gxp-boost"))
        {
            int64 balance = Shards_GetBalance(player->GetGUID().GetCounter());
            handler->PSendSysMessage(
                "|cffff0000[Conquest Shards]|r Not enough shards (have {}, need {}).",
                balance, amount);
            return true;
        }

        uint32 gxp = amount * GXP_PER_SHARD;
        GearTiers_AddGXP(player, gxp);

        handler->PSendSysMessage(
            "|cff9933ff[Conquest Shards]|r Spent {} shard(s) for {} Gear XP.", amount, gxp);
        return true;
    }

    // .shards log — last ~10 ledger rows for the calling player.
    static bool HandleShardsLog(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[Conquest Shards]|r Must be used in-game.");
            return true;
        }

        uint32 lowGuid = player->GetGUID().GetCounter();
        QueryResult result = CharacterDatabase.Query(
            "SELECT delta, reason, ts FROM conquest_shard_ledger "
            "WHERE guid = {} ORDER BY id DESC LIMIT 10", lowGuid);

        if (!result)
        {
            handler->SendSysMessage("|cff9933ff[Conquest Shards]|r No ledger entries.");
            return true;
        }

        handler->SendSysMessage("|cff9933ff[Conquest Shards] Recent Activity|r");
        do
        {
            Field* fields = result->Fetch();
            int64 delta          = fields[0].Get<int64>();
            std::string reason   = fields[1].Get<std::string>();
            std::string ts       = fields[2].Get<std::string>();

            handler->PSendSysMessage("  {}{} — {} ({})",
                delta >= 0 ? "+" : "", delta, reason, ts);
        } while (result->NextRow());

        return true;
    }
};

// ===========================================================================
// World Script
// ===========================================================================

class ConquestShardsWorldScript : public WorldScript
{
public:
    ConquestShardsWorldScript() : WorldScript("ConquestShardsWorldScript") {}

    void OnStartup() override
    {
        LOG_INFO("module", "[mod-conquest-shards] Module loaded.");
    }
};

// ===========================================================================
// Registration
// ===========================================================================

void AddSC_mod_conquest_shards()
{
    new ConquestShardsPlayerScript();
    new ConquestShardsCommandScript();
    new ConquestShardsWorldScript();
}
