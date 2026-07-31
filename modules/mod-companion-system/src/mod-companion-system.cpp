// mod-companion-system.cpp
//
// Sanctum "The Band" — PHASE 1 FRAMEWORK.
// -----------------------------------------------------------------------
// Four named, single-class, race-authentic companion NPCs Donnie can call
// to follow him everywhere (hub, world, dungeons via the normal Portal
// Master flow) and fight at his side. This is a personal/meaningful
// feature (see memory/project_companion_band.md) — the roster models his
// old raid/friend group.
//
// Roster (locked):
//   Bigbilly  (Orc male, Prot Warrior)         -> TANK    -> entry 700270
//   Tumblerr  (Troll male, Shaman)             -> HEALER  -> entry 700271
//   Onusx     (Undead male, Rogue)             -> MELEE   -> entry 700272
//   Denziel   (Tauren male, BM Hunter)         -> RANGED  -> entry 700273
//     -> pet Mazzranache, reuses the REAL Mulgore tallstrider creature
//        entry 3068 directly (no new entry needed).
//
// Architecture: mirrors modules/mod-pet-systems/src/mod-pet-systems.cpp's
// guardian pattern almost exactly (SummonCreature, per-player GUID
// tracking, REACT_AGGRESSIVE, MoveFollow, periodic WorldScript re-summon
// so a companion that gets "left behind" by a map change/teleport simply
// reappears next tick because the old GUID resolves to nullptr on the new
// map). No core edits; no new hooks beyond WorldScript/PlayerScript/
// CreatureScript, same as every other Sanctum module.
//
// PHASE 1 SCOPE — functional baseline AI only (deep authentic rotations
// are Phase 2, one companion at a time, Bigbilly first):
//   TANK    — attacks the player's target, periodically taunts whatever is
//             currently hitting the player, high HP/armor.
//   HEALER  — every combat tick, heals whoever (player or band) is lowest
//             and below ~70% HP; otherwise lobs a nuke.
//   MELEE   — attacks the player's target in melee.
//   RANGED  — attacks the player's target and casts a ranged nuke; keeps
//             Mazzranache summoned at its side.
//
// Persistence: character_companion_active (characters DB) records which
// companions a player has "called" so the band survives a restart and
// re-summons on login — see SQL/characters/base/mod_companion_system.sql.
//
// Gossip flow: each companion, while idle in the hub, offers "Come with
// me." / "Head back to Sanctum." (per-companion) plus a "Call the whole
// band." convenience option. No spawn rows are shipped — Donnie places
// each with `.npc add <entry>` per the locked Sanctum NPC-placement
// workflow (see SQL/world/base/mod_companion_system.sql).

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Unit.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "MotionMaster.h"
#include "GossipDef.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "SpellMgr.h"
#include "WorldSessionMgr.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <algorithm>

// ============================================================
// Roster definitions
// ============================================================

static const uint32 ENTRY_BIGBILLY    = 700270;
static const uint32 ENTRY_TUMBLERR    = 700271;
static const uint32 ENTRY_ONUSX       = 700272;
static const uint32 ENTRY_DENZIEL     = 700273;
static const uint32 ENTRY_MAZZRANACHE = 3068; // real Mulgore tallstrider — reused directly, no new entry

enum CompanionRole : uint8
{
    ROLE_TANK   = 1,
    ROLE_HEALER = 2,
    ROLE_MELEE  = 3,
    ROLE_RANGED = 4
};

struct CompanionDef
{
    uint32        entry;
    CompanionRole role;
    const char*   name;
};

static const CompanionDef COMPANION_DEFS[] =
{
    { ENTRY_BIGBILLY, ROLE_TANK,   "Bigbilly" },
    { ENTRY_TUMBLERR, ROLE_HEALER, "Tumblerr" },
    { ENTRY_ONUSX,    ROLE_MELEE,  "Onusx"    },
    { ENTRY_DENZIEL,  ROLE_RANGED, "Denziel"  },
};

static CompanionRole RoleForEntry(uint32 entry)
{
    for (auto const& d : COMPANION_DEFS)
        if (d.entry == entry)
            return d.role;
    return ROLE_MELEE;
}

// ============================================================
// Follow distances — ranged/healer hang back a bit, tank/melee stay tight.
// ============================================================

static const float FOLLOW_DIST_MELEE  = 1.5f;
static const float FOLLOW_DIST_RANGED = 6.0f;
static const float FOLLOW_ANGLE       = static_cast<float>(M_PI);

// Party formation (Donnie's ask): the tank (Bigbilly) and rogue (Onusx) walk
// ALONGSIDE the player (left/right flanks); the healer (Tumblerr) and hunter
// (Denziel) trail BEHIND. MoveFollow's angle is relative to the player's facing
// (M_PI = directly behind, +/-M_PI/2 = the sides).
static float FollowAngleForEntry(uint32 entry)
{
    switch (entry)
    {
        case ENTRY_BIGBILLY: return static_cast<float>(M_PI / 2.0);       // right flank, alongside
        case ENTRY_ONUSX:    return static_cast<float>(-M_PI / 2.0);      // left flank, alongside
        case ENTRY_TUMBLERR: return static_cast<float>(M_PI - 0.45);      // behind, offset one way
        case ENTRY_DENZIEL:  return static_cast<float>(M_PI + 0.45);      // behind, offset the other
        default:             return FOLLOW_ANGLE;
    }
}
static float FollowDistForEntry(uint32 entry)
{
    switch (entry)
    {
        case ENTRY_BIGBILLY: return 2.0f;   // alongside, tight
        case ENTRY_ONUSX:    return 2.0f;
        case ENTRY_TUMBLERR: return 3.0f;   // behind
        case ENTRY_DENZIEL:  return 4.0f;   // behind, ranged a touch further
        default:             return FOLLOW_DIST_MELEE;
    }
}

// ============================================================
// Spell IDs — functional-baseline abilities.
// Rank tables are best-effort (Phase 1 baseline, not tuned); Phase 2 will
// replace these with real per-character rotations.
// ============================================================

static const uint32 SPELL_TAUNT       = 355;   // Warrior Taunt
static const uint32 SPELL_ARCANE_SHOT = 3044;  // Denziel's ranged nuke (rank 1, fixed)

struct RankStep { uint8 minLevel; uint32 spellId; };

static const RankStep HEALING_WAVE_RANKS[] =
{
    { 1, 331 }, { 10, 332 }, { 20, 547 }, { 30, 939 }, { 40, 10395 }, { 50, 25357 }, { 60, 25396 },
};
static const RankStep LIGHTNING_BOLT_RANKS[] =
{
    { 1, 403 }, { 10, 529 }, { 20, 915 }, { 30, 6041 }, { 40, 10391 }, { 50, 15207 }, { 60, 25449 },
};

template <size_t N>
static uint32 PickRank(const RankStep (&table)[N], uint8 level)
{
    uint32 chosen = table[0].spellId;
    for (size_t i = 0; i < N; ++i)
    {
        if (level >= table[i].minLevel)
            chosen = table[i].spellId;
        else
            break;
    }
    return chosen;
}

// ============================================================
// Per-player tracking (mirrors mod-pet-systems' s_guardianGuids exactly)
// ============================================================

// lowguid -> companion entry -> creature GUID
static std::unordered_map<uint32, std::unordered_map<uint32, ObjectGuid>> s_companionGuids;
// lowguid -> Mazzranache's GUID (only meaningful while Denziel is active)
static std::unordered_map<uint32, ObjectGuid> s_mazzGuid;
// lowguid -> set of companion entries the player has "called" (cache of the DB table)
static std::unordered_map<uint32, std::unordered_set<uint32>> s_activeSet;
// creature lowguid -> ms remaining before that companion may taunt/nuke again
static std::unordered_map<uint32, uint32> s_abilityCd;

// ============================================================
// Persistence — character_companion_active
// ============================================================

static void DB_AddActive(uint32 lguid, uint32 entry)
{
    CharacterDatabase.Execute(
        "INSERT IGNORE INTO character_companion_active (guid, companion_entry) VALUES ({}, {})",
        lguid, entry);
}

static void DB_RemoveActive(uint32 lguid, uint32 entry)
{
    CharacterDatabase.Execute(
        "DELETE FROM character_companion_active WHERE guid = {} AND companion_entry = {}",
        lguid, entry);
}

static std::unordered_set<uint32> DB_LoadActive(uint32 lguid)
{
    std::unordered_set<uint32> result;
    QueryResult r = CharacterDatabase.Query(
        "SELECT companion_entry FROM character_companion_active WHERE guid = {}", lguid);
    if (r)
    {
        do
        {
            result.insert(r->Fetch()[0].Get<uint32>());
        } while (r->NextRow());
    }
    return result;
}

// ============================================================
// Stat scaling — level-anchored to the player, scaled by role.
// Mirrors mod-pet-systems' ApplyOwnerAndBagStatsToUnit pattern
// (HandleStatFlatModifier + UpdateAllStats/UpdateMaxHealth).
// ============================================================

static void ApplyCompanionRoleStats(Player* player, Creature* c, CompanionRole role)
{
    float str   = player->GetStat(STAT_STRENGTH);
    float agi   = player->GetStat(STAT_AGILITY);
    float sta   = player->GetStat(STAT_STAMINA);
    float intel = player->GetStat(STAT_INTELLECT);
    float spi   = player->GetStat(STAT_SPIRIT);
    float ap    = player->GetTotalAttackPowerValue(BASE_ATTACK);

    float staMul, strMul, agiMul, intMul, spiMul, apMul, armorMul;
    switch (role)
    {
        case ROLE_TANK:
            staMul = 1.6f; strMul = 1.2f; agiMul = 0.8f; intMul = 0.4f; spiMul = 0.4f; apMul = 0.6f; armorMul = 1.5f;
            break;
        case ROLE_HEALER:
            staMul = 1.0f; strMul = 0.5f; agiMul = 0.5f; intMul = 1.4f; spiMul = 1.4f; apMul = 0.5f; armorMul = 0.3f;
            break;
        case ROLE_RANGED:
            staMul = 0.9f; strMul = 0.6f; agiMul = 1.3f; intMul = 0.5f; spiMul = 0.5f; apMul = 0.7f; armorMul = 0.5f;
            break;
        case ROLE_MELEE:
        default:
            staMul = 0.9f; strMul = 1.0f; agiMul = 1.3f; intMul = 0.4f; spiMul = 0.4f; apMul = 0.7f; armorMul = 0.5f;
            break;
    }

    c->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  BASE_VALUE, str   * strMul, true);
    c->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   BASE_VALUE, agi   * agiMul, true);
    c->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   BASE_VALUE, sta   * staMul, true);
    c->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, BASE_VALUE, intel * intMul, true);
    c->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    BASE_VALUE, spi   * spiMul, true);
    c->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER,   BASE_VALUE, ap    * apMul,  true);
    c->UpdateAllStats();
    c->UpdateAttackPowerAndDamage();

    // HP: Stamina-derived (10 HP/point, matches mod-pet-systems' ratio) plus a
    // flat slice of the player's own max HP so tanks in particular are sturdy
    // from level 1 even before Stamina scales up.
    float hp = sta * staMul * 10.0f + player->GetMaxHealth() * (role == ROLE_TANK ? 0.6f : 0.3f);
    c->HandleStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, hp, true);
    c->UpdateMaxHealth();
    c->SetFullHealth();

    if (role == ROLE_HEALER)
    {
        uint32 mana = 1000 + player->GetMaxPower(POWER_MANA);
        c->SetMaxPower(POWER_MANA, mana);
        c->SetPower(POWER_MANA, mana);
    }

    // Armor — boost physical (NORMAL school) resistance directly.
    int32 baseArmor  = c->GetResistance(SPELL_SCHOOL_NORMAL);
    int32 refArmor   = player->GetArmor() > 0 ? player->GetArmor() : 500;
    int32 bonusArmor = static_cast<int32>(refArmor * armorMul);
    c->SetResistance(SPELL_SCHOOL_NORMAL, baseArmor + bonusArmor);
}

// Weapon damage scaled as a % of player max HP, same technique as
// mod-pet-systems' ApplyGuardianStatScaling (SelectLevel() locks template
// damage otherwise).
static void ApplyCompanionWeaponScaling(Player* player, Creature* c, CompanionRole role)
{
    float hp = static_cast<float>(player->GetMaxHealth());
    float minPct, maxPct;
    switch (role)
    {
        // Tuned DOWN (2026-07-23) — companions were meleeing too hard / carrying
        // the fight. They should help, not trivialize. ~half the prior values.
        case ROLE_TANK:   minPct = 0.010f; maxPct = 0.018f; break;
        case ROLE_MELEE:  minPct = 0.018f; maxPct = 0.030f; break;
        case ROLE_RANGED: minPct = 0.016f; maxPct = 0.028f; break;
        case ROLE_HEALER:
        default:          minPct = 0.008f; maxPct = 0.014f; break;
    }

    float minDmg = std::max(hp * minPct, 1.0f);
    float maxDmg = std::max(hp * maxPct, 2.0f);

    c->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, minDmg);
    c->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, maxDmg);
    c->UpdateDamagePhysical(BASE_ATTACK);
}

// ============================================================
// Summon / despawn helpers
// ============================================================

static void SummonMazzranache(Player* player, Creature* denziel)
{
    uint32 lguid = player->GetGUID().GetCounter();

    auto it = s_mazzGuid.find(lguid);
    if (it != s_mazzGuid.end())
    {
        if (Creature* old = ObjectAccessor::GetCreature(*player, it->second))
            old->DespawnOrUnsummon();
        s_mazzGuid.erase(it);
    }

    float x, y, z;
    denziel->GetClosePoint(x, y, z, denziel->GetObjectSize(), 2.0f);
    float o = denziel->GetOrientation();

    Creature* pet = player->SummonCreature(ENTRY_MAZZRANACHE, x, y, z, o, TEMPSUMMON_MANUAL_DESPAWN, 0);
    if (!pet)
        return;

    pet->SetOwnerGUID(player->GetGUID());
    pet->SetFaction(player->GetFaction());
    pet->SetLevel(std::max<uint8>(player->GetLevel(), 1));
    pet->SetReactState(REACT_AGGRESSIVE);
    ApplyCompanionRoleStats(player, pet, ROLE_RANGED);
    ApplyCompanionWeaponScaling(player, pet, ROLE_RANGED);
    pet->SetHomePosition(x, y, z, o);
    pet->GetMotionMaster()->MoveFollow(denziel, 2.0f, FOLLOW_ANGLE);

    s_mazzGuid[lguid] = pet->GetGUID();
}

static Creature* SummonCompanion(Player* player, uint32 entry)
{
    uint32 lguid = player->GetGUID().GetCounter();

    // Despawn any stale tracked instance first (same guard as SummonCombatGuardian).
    auto pIt = s_companionGuids.find(lguid);
    if (pIt != s_companionGuids.end())
    {
        auto eIt = pIt->second.find(entry);
        if (eIt != pIt->second.end())
        {
            if (Creature* old = ObjectAccessor::GetCreature(*player, eIt->second))
                old->DespawnOrUnsummon();
            pIt->second.erase(eIt);
        }
    }

    float x, y, z;
    player->GetClosePoint(x, y, z, player->GetObjectSize(), 2.0f);
    float o = player->GetOrientation();

    Creature* c = player->SummonCreature(entry, x, y, z, o, TEMPSUMMON_MANUAL_DESPAWN, 0);
    if (!c)
        return nullptr;

    c->SetOwnerGUID(player->GetGUID());
    c->SetFaction(player->GetFaction());
    c->SetLevel(std::max<uint8>(player->GetLevel(), 1));
    c->SetReactState(REACT_AGGRESSIVE);

    CompanionRole role = RoleForEntry(entry);
    ApplyCompanionRoleStats(player, c, role);
    ApplyCompanionWeaponScaling(player, c, role);

    // Denziel carries a bow so he looks — and fights — like the hunter he is.
    if (entry == ENTRY_DENZIEL)
        c->SetVirtualItem(2, 2508); // 2 = ranged slot; 2508 = Large Recurve Bow

    c->SetHomePosition(x, y, z, o);
    c->GetMotionMaster()->MoveFollow(player, FollowDistForEntry(entry), FollowAngleForEntry(entry));

    s_companionGuids[lguid][entry] = c->GetGUID();

    if (entry == ENTRY_DENZIEL)
        SummonMazzranache(player, c);

    return c;
}

static bool IsCompanionAlive(Player* player, uint32 entry)
{
    uint32 lguid = player->GetGUID().GetCounter();
    auto pIt = s_companionGuids.find(lguid);
    if (pIt == s_companionGuids.end())
        return false;
    auto eIt = pIt->second.find(entry);
    if (eIt == pIt->second.end())
        return false;
    Creature* c = ObjectAccessor::GetCreature(*player, eIt->second);
    return c && c->IsAlive();
}

static void DespawnCompanion(Player* player, uint32 entry)
{
    uint32 lguid = player->GetGUID().GetCounter();

    auto pIt = s_companionGuids.find(lguid);
    if (pIt != s_companionGuids.end())
    {
        auto eIt = pIt->second.find(entry);
        if (eIt != pIt->second.end())
        {
            if (Creature* c = ObjectAccessor::GetCreature(*player, eIt->second))
                c->DespawnOrUnsummon();
            pIt->second.erase(eIt);
        }
    }

    if (entry == ENTRY_DENZIEL)
    {
        auto mIt = s_mazzGuid.find(lguid);
        if (mIt != s_mazzGuid.end())
        {
            if (Creature* p = ObjectAccessor::GetCreature(*player, mIt->second))
                p->DespawnOrUnsummon();
            s_mazzGuid.erase(mIt);
        }
    }
}

static void DespawnAllCompanions(Player* player)
{
    uint32 lguid = player->GetGUID().GetCounter();

    auto pIt = s_companionGuids.find(lguid);
    if (pIt != s_companionGuids.end())
    {
        for (auto& [entry, guid] : pIt->second)
        {
            if (Creature* c = ObjectAccessor::GetCreature(*player, guid))
                c->DespawnOrUnsummon();
        }
        s_companionGuids.erase(pIt);
    }

    auto mIt = s_mazzGuid.find(lguid);
    if (mIt != s_mazzGuid.end())
    {
        if (Creature* p = ObjectAccessor::GetCreature(*player, mIt->second))
            p->DespawnOrUnsummon();
        s_mazzGuid.erase(mIt);
    }
}

// ============================================================
// Combat/behavior tick — functional baseline per role.
// ============================================================

static Unit* FindLowestHpFriendly(Player* player)
{
    Unit* best = nullptr;
    float bestPct = 0.70f; // only heal below 70% HP

    auto consider = [&](Unit* u)
    {
        if (!u || !u->IsAlive())
            return;
        uint32 maxHp = u->GetMaxHealth();
        if (!maxHp)
            return;
        float pct = static_cast<float>(u->GetHealth()) / static_cast<float>(maxHp);
        if (pct < bestPct)
        {
            bestPct = pct;
            best = u;
        }
    };

    consider(player);

    uint32 lguid = player->GetGUID().GetCounter();
    auto cIt = s_companionGuids.find(lguid);
    if (cIt != s_companionGuids.end())
        for (auto& [entry, guid] : cIt->second)
            consider(ObjectAccessor::GetCreature(*player, guid));

    auto mIt = s_mazzGuid.find(lguid);
    if (mIt != s_mazzGuid.end())
        consider(ObjectAccessor::GetCreature(*player, mIt->second));

    return best;
}

static void TickAbilityCooldown(uint32 creatureLguid, uint32 elapsedMs)
{
    auto it = s_abilityCd.find(creatureLguid);
    if (it == s_abilityCd.end())
        return;
    it->second = (it->second > elapsedMs) ? it->second - elapsedMs : 0;
}

static bool AbilityReady(uint32 creatureLguid)
{
    auto it = s_abilityCd.find(creatureLguid);
    return it == s_abilityCd.end() || it->second == 0;
}

static void SetAbilityCooldown(uint32 creatureLguid, uint32 ms)
{
    s_abilityCd[creatureLguid] = ms;
}

// Tank taunt: pick whatever is currently attacking the player and taunt it off.
static void DoTauntCheck(Player* player, Creature* tank)
{
    uint32 tguid = tank->GetGUID().GetCounter();
    if (!AbilityReady(tguid))
        return;

    for (Unit* attacker : player->getAttackers())
    {
        if (!attacker || !attacker->IsAlive())
            continue;
        if (!tank->IsValidAttackTarget(attacker))
            continue;
        if (!tank->IsWithinDistInMap(attacker, 30.0f))
            continue;

        tank->CastSpell(attacker, SPELL_TAUNT, false);
        SetAbilityCooldown(tguid, 6000);
        return;
    }
}

static void RunCompanionCombatTick(Player* player, Creature* c, CompanionRole role, uint32 elapsedMs)
{
    uint32 cguid = c->GetGUID().GetCounter();
    TickAbilityCooldown(cguid, elapsedMs);

    // Keep home anchored to the player so evade never sends the companion
    // back to a stale spawn point (same reasoning as mod-pet-systems).
    c->SetHomePosition(player->GetPositionX(), player->GetPositionY(),
                        player->GetPositionZ(), player->GetOrientation());

    // Healer: triage first, every tick, regardless of the player's combat state.
    if (role == ROLE_HEALER)
    {
        if (Unit* healTarget = FindLowestHpFriendly(player))
        {
            if (c->IsWithinDistInMap(healTarget, 40.0f))
            {
                uint32 spellId = PickRank(HEALING_WAVE_RANKS, c->GetLevel());
                c->InterruptNonMeleeSpells(false);
                c->CastSpell(healTarget, spellId, false);
                return; // heal priority — skip offense this tick
            }
        }
    }

    if (!player->IsInCombat())
    {
        if (!c->IsInCombat())
        {
            c->GetMotionMaster()->Clear();
            c->GetMotionMaster()->MoveFollow(player, FollowDistForEntry(c->GetEntry()), FollowAngleForEntry(c->GetEntry()));
        }
        return;
    }

    Unit* target = player->GetVictim();
    if (!target || !target->IsAlive() || !c->IsValidAttackTarget(target))
        target = c->GetVictim();
    if (!target || !target->IsAlive())
        return;

    switch (role)
    {
        case ROLE_TANK:
            if (c->GetVictim() != target)
            {
                c->GetMotionMaster()->Clear();
                c->AI()->AttackStart(target);
            }
            DoTauntCheck(player, c);
            break;

        case ROLE_MELEE:
            if (c->GetVictim() != target)
            {
                c->GetMotionMaster()->Clear();
                c->AI()->AttackStart(target);
            }
            break;

        case ROLE_RANGED:
            // Denziel is a HUNTER — hold at range and shoot, never melee-charge.
            if (c->GetVictim() != target)
            {
                c->SetTarget(target->GetGUID());
                c->Attack(target, false);   // false = RANGED auto-attack (uses his bow)
            }
            {
                float dist = c->GetDistance(target);
                if (dist < 15.0f || dist > 30.0f)
                    c->GetMotionMaster()->MoveChase(target, 22.0f); // maintain ~22yd
            }
            c->SetFacingToObject(target);
            if (AbilityReady(cguid) && c->IsWithinDistInMap(target, 35.0f))
            {
                c->CastSpell(target, SPELL_ARCANE_SHOT, false);
                SetAbilityCooldown(cguid, 3000);
            }
            break;

        case ROLE_HEALER:
            if (AbilityReady(cguid) && c->IsWithinDistInMap(target, 30.0f))
            {
                uint32 spellId = PickRank(LIGHTNING_BOLT_RANKS, c->GetLevel());
                c->CastSpell(target, spellId, false);
                SetAbilityCooldown(cguid, 3000);
            }
            else if (c->GetVictim() != target)
            {
                c->GetMotionMaster()->Clear();
                c->AI()->AttackStart(target);
            }
            break;
    }
}

// ============================================================
// PlayerScript — login/logout/level-up
// ============================================================

class CompanionPlayerScript : public PlayerScript
{
public:
    CompanionPlayerScript() : PlayerScript("CompanionPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player || !player->IsInWorld())
            return;

        uint32 lguid = player->GetGUID().GetCounter();
        s_activeSet[lguid] = DB_LoadActive(lguid);
        for (uint32 entry : s_activeSet[lguid])
            SummonCompanion(player, entry);
    }

    void OnPlayerLogout(Player* player) override
    {
        DespawnAllCompanions(player);
        s_activeSet.erase(player->GetGUID().GetCounter());
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!player || !player->IsInWorld())
            return;

        uint32 lguid = player->GetGUID().GetCounter();
        auto it = s_activeSet.find(lguid);
        if (it == s_activeSet.end() || it->second.empty())
            return;

        // Despawn and re-summon fresh so level/stat scaling is re-applied cleanly.
        DespawnAllCompanions(player);
        for (uint32 entry : it->second)
            SummonCompanion(player, entry);
    }
};

// ============================================================
// WorldScript — periodic tick.
// 10s: re-summon any active companion that isn't alive/present (this is
//      what makes "follow everywhere" work — a teleport/map change leaves
//      the old creature unreachable via ObjectAccessor, so the very next
//      tick treats it as missing and summons a fresh one at the player's
//      new location, exactly like mod-pet-systems' guardian re-summon).
// 2s:  combat/behavior tick per active companion.
// ============================================================

class CompanionWorldScript : public WorldScript
{
public:
    CompanionWorldScript() : WorldScript("CompanionWorldScript") {}

    void OnUpdate(uint32 diff) override
    {
        _combatTimer += diff;
        _summonTimer += diff;

        bool doCombat = (_combatTimer >= 2000);
        bool doSummon = (_summonTimer >= 10000);
        if (!doCombat && !doSummon)
            return;

        uint32 combatElapsed = doCombat ? _combatTimer : 0;
        if (doCombat) _combatTimer = 0;
        if (doSummon) _summonTimer = 0;

        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            if (!session) continue;
            Player* player = session->GetPlayer();
            if (!player || !player->IsInWorld()) continue;

            uint32 lguid = player->GetGUID().GetCounter();
            auto activeIt = s_activeSet.find(lguid);
            if (activeIt == s_activeSet.end() || activeIt->second.empty())
                continue;

            if (!player->IsAlive())
            {
                if (doSummon)
                    DespawnAllCompanions(player); // band re-appears once the player is alive again
                continue;
            }

            if (doSummon)
            {
                for (uint32 entry : activeIt->second)
                    if (!IsCompanionAlive(player, entry))
                        SummonCompanion(player, entry);

                if (activeIt->second.count(ENTRY_DENZIEL))
                {
                    auto mIt = s_mazzGuid.find(lguid);
                    bool mazzAlive = (mIt != s_mazzGuid.end()) &&
                        ObjectAccessor::GetCreature(*player, mIt->second) &&
                        ObjectAccessor::GetCreature(*player, mIt->second)->IsAlive();
                    if (!mazzAlive)
                    {
                        auto cIt = s_companionGuids.find(lguid);
                        if (cIt != s_companionGuids.end())
                        {
                            auto eIt = cIt->second.find(ENTRY_DENZIEL);
                            if (eIt != cIt->second.end())
                                if (Creature* denziel = ObjectAccessor::GetCreature(*player, eIt->second))
                                    SummonMazzranache(player, denziel);
                        }
                    }
                }
            }

            if (doCombat)
            {
                auto cIt = s_companionGuids.find(lguid);
                if (cIt == s_companionGuids.end())
                    continue;
                for (auto& [entry, guid] : cIt->second)
                {
                    Creature* c = ObjectAccessor::GetCreature(*player, guid);
                    if (!c || !c->IsAlive()) continue;
                    RunCompanionCombatTick(player, c, RoleForEntry(entry), combatElapsed);
                }
            }
        }
    }

private:
    uint32 _combatTimer = 0;
    uint32 _summonTimer = 0;
};

// ============================================================
// Gossip NPC — one CreatureScript, ScriptName shared by all 4 entries.
// "Come with me." / "Head back to Sanctum." per-companion, plus a
// "Call the whole band." convenience option.
// ============================================================

class npc_sanctum_companion : public CreatureScript
{
public:
    npc_sanctum_companion() : CreatureScript("npc_sanctum_companion") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ShowMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        uint32 entry = creature->GetEntry();
        uint32 lguid = player->GetGUID().GetCounter();

        switch (action)
        {
            case 1: // Come with me.
                if (s_activeSet[lguid].insert(entry).second)
                {
                    DB_AddActive(lguid, entry);
                    SummonCompanion(player, entry);
                }
                break;

            case 2: // Head back to Sanctum.
                if (s_activeSet[lguid].erase(entry))
                {
                    DB_RemoveActive(lguid, entry);
                    DespawnCompanion(player, entry);
                }
                break;

            case 3: // Call the whole band.
                for (auto const& d : COMPANION_DEFS)
                {
                    if (s_activeSet[lguid].insert(d.entry).second)
                    {
                        DB_AddActive(lguid, d.entry);
                        SummonCompanion(player, d.entry);
                    }
                }
                break;

            default:
                break;
        }

        CloseGossipMenuFor(player);
        return true;
    }

private:
    void ShowMenu(Player* player, Creature* creature)
    {
        ClearGossipMenuFor(player);
        uint32 entry = creature->GetEntry();
        uint32 lguid = player->GetGUID().GetCounter();
        bool active = s_activeSet[lguid].count(entry) > 0;

        if (!active)
            AddGossipItemFor(player, GOSSIP_ICON_TALK, "Come with me.", GOSSIP_SENDER_MAIN, 1);
        else
            AddGossipItemFor(player, GOSSIP_ICON_TALK, "Head back to Sanctum.", GOSSIP_SENDER_MAIN, 2);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Call the whole band.", GOSSIP_SENDER_MAIN, 3);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Nevermind.", GOSSIP_SENDER_MAIN, 9);

        SendGossipMenuFor(player, entry, creature->GetGUID()); // npc_text.ID == entry
    }
};

// ============================================================
// Script registration
// ============================================================

void AddSC_mod_companion_system()
{
    LOG_INFO("server.loading", "[mod-companion-system] Module loaded.");
    new CompanionPlayerScript();
    new CompanionWorldScript();
    new npc_sanctum_companion();
}
