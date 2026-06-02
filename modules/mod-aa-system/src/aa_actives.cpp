// aa_actives.cpp
//
// Sanctum AA — activate (on-demand) ability handlers.
// Called via ".aa use <id>" command dispatched from mod-aa-system.cpp.
//
// Cooldowns are tracked per-GUID per-aaId in g_activeCDs.
// SanctumAA_ClearActivateState() is called on logout and death.
//
// Batch 3 status:
//   IMPLEMENTED:  Rampage, Warcry, Death Blow, Escape Artist, Dancing Blade,
//                 Force of Will, Divine Stun, Invocation, DK Lifeburn,
//                 Death Pact, DK Leech Touch, Cannibalize, Elemental Fury,
//                 Harvest of Druzzil, Manaburn, Fearstorm, WRL Lifeburn,
//                 WRL Leech Touch
//   STUBBED:      Volley Burst, Scout of the Wild, Assassin's Mark, all
//                 remaining Priest actives (5403-5409,5418,5420-5421,5424,5426),
//                 Frenzied Burnout, Mend Companion, Wake the Dead, Dire Charm

#include "aa_runtime.h"
#include "Player.h"
#include "Unit.h"
#include "Creature.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "SharedDefines.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

// ---- cooldown state -------------------------------------------------------

// [playerGuid][aaId] = getMSTime() at last use
static std::unordered_map<uint32, std::unordered_map<uint32, uint32>> g_activeCDs;

// Returns ms remaining on cooldown; 0 if ready to use.
static uint32 CDRemaining(uint32 guid, uint32 aaId, uint32 cdMs)
{
    auto git = g_activeCDs.find(guid);
    if (git == g_activeCDs.end()) return 0;
    auto it = git->second.find(aaId);
    if (it == git->second.end()) return 0;
    uint32 elapsed = GetMSTimeDiffToNow(it->second);
    return (elapsed < cdMs) ? (cdMs - elapsed) : 0;
}

static void SetCD(uint32 guid, uint32 aaId)
{
    g_activeCDs[guid][aaId] = getMSTime();
}

// ---- combat helpers -------------------------------------------------------

static Unit* GetTarget(Player* player)
{
    if (Unit* v = player->GetVictim())
        return v;
    return ObjectAccessor::GetUnit(*player, player->GetTarget());
}

static bool IsEliteOrBoss(Unit* u)
{
    if (Creature const* cr = u->ToCreature())
        return cr->isElite() || cr->IsDungeonBoss();
    return false;
}

// Collect attackers + current target within range into a snapshot so
// iteration is safe even if DealDamage causes unit deaths mid-loop.
static std::vector<Unit*> NearbyEnemies(Player* player, float range)
{
    std::vector<Unit*> out;
    for (Unit* atk : player->getAttackers())
    {
        if (atk->IsAlive() && player->GetDistance(atk) <= range)
            out.push_back(atk);
    }
    Unit* tgt = GetTarget(player);
    if (tgt && tgt->IsAlive() && player->IsValidAttackTarget(tgt) &&
        player->GetDistance(tgt) <= range)
    {
        if (std::find(out.begin(), out.end(), tgt) == out.end())
            out.push_back(tgt);
    }
    return out;
}

// ---- public API -----------------------------------------------------------

void SanctumAA_ClearActivateState(uint32 guid)
{
    g_activeCDs.erase(guid);
}

bool SanctumAA_HandleActivate(Player* player, uint32 aaId, ChatHandler* handler)
{
    if (!player)
        return true;

    uint32 guid = player->GetGUID().GetCounter();
    uint8  rank = SanctumAA::GetRank(player, aaId);
    if (rank == 0)
    {
        handler->SendSysMessage("|cffff0000[AA]|r You haven't purchased that ability.");
        return true;
    }

    switch (aaId)
    {
    // =======================================================================
    // WARRIOR
    // =======================================================================

    case AA_WAR_RAMPAGE:
    // Activate: strike all enemies within 8 yards for 100/130/160% weapon dmg.
    // 30s/25s/20s CD. 20 rage cost.
    {
        static const uint32 cdMs[] = { 0, 30000, 25000, 20000 };
        uint32 cd = cdMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Rampage on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        if (player->GetPower(POWER_RAGE) < 200)  // 20 rage = 200 internal units
        {
            handler->SendSysMessage("|cffff0000[AA]|r Not enough Rage (needs 20).");
            return true;
        }
        static const float mult[] = { 0.0f, 1.0f, 1.3f, 1.6f };
        uint32 dmg = (uint32)(player->GetTotalAttackPowerValue(BASE_ATTACK) / 14.0f * mult[rank]);
        for (Unit* u : NearbyEnemies(player, 8.0f))
            Unit::DealDamage(player, u, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        player->SetPower(POWER_RAGE, player->GetPower(POWER_RAGE) - 200);
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Rampage!");
        return true;
    }

    case AA_WAR_WARCRY:
    // Activate: fear immunity for 10s via Berserker Rage (18499). 90s CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 90000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Warcry on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        player->CastSpell(player, 18499, true);  // Berserker Rage: fear immunity 10s
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Warcry! Fear immunity for 10 seconds.");
        return true;
    }

    // =======================================================================
    // ROGUE
    // =======================================================================

    case AA_ROG_DEATH_BLOW:
    // Activate: 300% weapon dmg. Instantly kills targets at or below 15% HP. 3min CD. Melee range.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 180000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Death Blow on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        if (player->GetDistance(tgt) > 5.0f)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Target is out of melee range.");
            return true;
        }
        SetCD(guid, aaId);
        if (tgt->GetHealthPct() <= 15.0f)
        {
            Unit::DealDamage(player, tgt, tgt->GetHealth(), nullptr, DIRECT_DAMAGE,
                             SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            handler->SendSysMessage("|cff00ff00[AA]|r Death Blow — killing blow!");
        }
        else
        {
            uint32 dmg = (uint32)(player->GetTotalAttackPowerValue(BASE_ATTACK) / 14.0f * 3.0f);
            Unit::DealDamage(player, tgt, dmg, nullptr, DIRECT_DAMAGE,
                             SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
            handler->SendSysMessage("|cff00ff00[AA]|r Death Blow!");
        }
        return true;
    }

    case AA_ROG_ESCAPE_ARTIST:
    // Activate: remove all roots, snares, and movement-impairing effects. 45/35/25s CD.
    {
        static const uint32 cdMs[] = { 0, 45000, 35000, 25000 };
        uint32 cd = cdMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Escape Artist on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        uint32 moveMask = (1u << MECHANIC_ROOT) | (1u << MECHANIC_SNARE) |
                          (1u << MECHANIC_DAZE) | (1u << MECHANIC_FREEZE);
        player->RemoveAurasWithMechanic(moveMask);
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Escape Artist — movement impairments removed.");
        return true;
    }

    case AA_ROG_DANCING_BLADE:
    // Activate: strike all enemies within 6 yards for 80/110/140% weapon dmg. 20s CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 20000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Dancing Blade on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const float mult[] = { 0.0f, 0.8f, 1.1f, 1.4f };
        uint32 dmg = (uint32)(player->GetTotalAttackPowerValue(BASE_ATTACK) / 14.0f * mult[rank]);
        for (Unit* u : NearbyEnemies(player, 6.0f))
            Unit::DealDamage(player, u, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Dancing Blade!");
        return true;
    }

    // =======================================================================
    // PRIEST
    // =======================================================================

    case AA_PRI_FORCE_OF_WILL:
    // Activate: instantly heal 20/35/50% max HP. 3min CD; halved to 90s below 20% HP.
    {
        bool   lowHP  = player->GetHealthPct() < 20.0f;
        uint32 cdMs   = lowHP ? 90000u : 180000u;
        if (uint32 rem = CDRemaining(guid, aaId, cdMs))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Force of Will on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const float pct[] = { 0.0f, 0.20f, 0.35f, 0.50f };
        int32 heal = (int32)(player->GetMaxHealth() * pct[rank]);
        player->ModifyHealth(heal);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Force of Will — healed for {}.", heal);
        return true;
    }

    case AA_PRI_DIVINE_STUN:
    // Activate: AoE stun all enemies within 8/10/12 yards for 2s. 60s CD. No bosses/elites.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 60000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Divine Stun on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        if (player->GetMaxPower(POWER_MANA) > 0)
        {
            uint32 manaCost = (uint32)(player->GetMaxPower(POWER_MANA) * 0.05f);
            if (player->GetPower(POWER_MANA) < (int32)manaCost)
            {
                handler->SendSysMessage("|cffff0000[AA]|r Not enough mana.");
                return true;
            }
            player->SetPower(POWER_MANA, player->GetPower(POWER_MANA) - (int32)manaCost);
        }
        static const float radii[] = { 0.0f, 8.0f, 10.0f, 12.0f };
        float radius = radii[std::min<uint8>(rank, 3)];
        uint8 count  = 0;
        for (Unit* u : NearbyEnemies(player, radius))
        {
            if (IsEliteOrBoss(u)) continue;
            player->CastSpell(u, 20549, true);  // War Stomp: 2s stun
            ++count;
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Divine Stun — {} target(s) stunned.", count);
        return true;
    }

    case AA_PRI_INVOCATION:
    // Activate: deal 100/150/200% SP as holy dmg to target; self-heal for same. 3min CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 180000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Invocation on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        static const float mult[] = { 0.0f, 1.0f, 1.5f, 2.0f };
        int32 sp  = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_HOLY);
        if (sp < 0) sp = 0;
        uint32 dmg = (uint32)(sp * mult[rank]);
        Unit::DealDamage(player, tgt, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_HOLY, nullptr, false);
        player->ModifyHealth((int32)dmg);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Invocation — {} holy dmg; {} HP restored.", dmg, dmg);
        return true;
    }

    // =======================================================================
    // DEATH KNIGHT
    // =======================================================================

    case AA_DK_LIFEBURN:
    // Activate: sacrifice 20% max HP; deal 100/150/200% AP as shadow dmg. 3min CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 180000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Lifeburn on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        uint32 sacrifice = (uint32)(player->GetMaxHealth() * 0.20f);
        if (player->GetHealth() <= sacrifice)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Not enough HP to sacrifice.");
            return true;
        }
        static const float mult[] = { 0.0f, 1.0f, 1.5f, 2.0f };
        uint32 dmg = (uint32)(player->GetTotalAttackPowerValue(BASE_ATTACK) * mult[rank]);
        player->ModifyHealth(-(int32)sacrifice);
        Unit::DealDamage(player, tgt, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Lifeburn — sacrificed {} HP, dealt {} shadow.", sacrifice, dmg);
        return true;
    }

    case AA_DK_DEATH_PACT:
    // Activate: sacrifice 15% max HP; generate 40/60/80 Runic Power. 45s CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 45000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Death Pact on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        if (player->GetMaxPower(POWER_RUNIC_POWER) == 0)
        {
            handler->SendSysMessage("|cffff0000[AA]|r You have no Runic Power resource.");
            return true;
        }
        uint32 sacrifice = (uint32)(player->GetMaxHealth() * 0.15f);
        if (player->GetHealth() <= sacrifice)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Not enough HP to sacrifice.");
            return true;
        }
        static const uint32 rpGain[] = { 0, 40, 60, 80 };
        uint32 rp = rpGain[std::min<uint8>(rank, 3)];
        player->ModifyHealth(-(int32)sacrifice);
        // RP stored internally as ×10
        int32 newRP = std::min(player->GetPower(POWER_RUNIC_POWER) + (int32)(rp * 10),
                               player->GetMaxPower(POWER_RUNIC_POWER));
        player->SetPower(POWER_RUNIC_POWER, newRP);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Death Pact — sacrificed {} HP, generated {} RP.", sacrifice, rp);
        return true;
    }

    case AA_DK_LEECH_TOUCH:
    // Activate: deal 15/25/40% target HP as shadow; self-heal same. 5/4/3 min CD. Melee range.
    {
        static const uint32 cdMs[] = { 0, 300000, 240000, 180000 };
        uint32 cd = cdMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Leech Touch on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        if (player->GetDistance(tgt) > 5.0f)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Target must be in melee range.");
            return true;
        }
        static const float pct[] = { 0.0f, 0.15f, 0.25f, 0.40f };
        uint32 dmg = std::max(1u, (uint32)(tgt->GetHealth() * pct[rank]));
        Unit::DealDamage(player, tgt, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
        player->ModifyHealth((int32)dmg);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Leech Touch — drained/healed {}.", dmg);
        return true;
    }

    // =======================================================================
    // SHAMAN
    // =======================================================================

    case AA_SHA_CANNIBALIZE:
    // Activate: sacrifice 15% max HP; restore 20/35/50% max mana. 45s CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 45000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Cannibalize on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        if (player->GetMaxPower(POWER_MANA) == 0)
        {
            handler->SendSysMessage("|cffff0000[AA]|r You have no mana resource.");
            return true;
        }
        uint32 sacrifice = (uint32)(player->GetMaxHealth() * 0.15f);
        if (player->GetHealth() <= sacrifice)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Not enough HP to sacrifice.");
            return true;
        }
        static const float pct[] = { 0.0f, 0.20f, 0.35f, 0.50f };
        uint32 manaGain = (uint32)(player->GetMaxPower(POWER_MANA) * pct[rank]);
        player->ModifyHealth(-(int32)sacrifice);
        int32 newMana = std::min(player->GetPower(POWER_MANA) + (int32)manaGain,
                                 player->GetMaxPower(POWER_MANA));
        player->SetPower(POWER_MANA, newMana);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Cannibalize — sacrificed {} HP, restored {} mana.", sacrifice, manaGain);
        return true;
    }

    case AA_SHA_ELEMENTAL_FURY:
    // Activate: nature AoE 150/225/300% SP to all enemies within 10 yards. 90s CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 90000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Elemental Fury on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const float mult[] = { 0.0f, 1.5f, 2.25f, 3.0f };
        int32 sp  = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE);
        if (sp < 0) sp = 0;
        uint32 dmg = (uint32)(sp * mult[rank]);
        for (Unit* u : NearbyEnemies(player, 10.0f))
            Unit::DealDamage(player, u, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NATURE, nullptr, false);
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Elemental Fury — unleashed!");
        return true;
    }

    // =======================================================================
    // MAGE
    // =======================================================================

    case AA_MAG_HARVEST_OF_DRUZZIL:
    // Activate: instantly restore 15/25/40% max mana. 8/6/4 min CD.
    {
        static const uint32 cdMs[] = { 0, 480000, 360000, 240000 };
        uint32 cd = cdMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Harvest of Druzzil on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        if (player->GetMaxPower(POWER_MANA) == 0)
        {
            handler->SendSysMessage("|cffff0000[AA]|r You have no mana resource.");
            return true;
        }
        static const float pct[] = { 0.0f, 0.15f, 0.25f, 0.40f };
        uint32 manaGain = (uint32)(player->GetMaxPower(POWER_MANA) * pct[rank]);
        int32  newMana  = std::min(player->GetPower(POWER_MANA) + (int32)manaGain,
                                   player->GetMaxPower(POWER_MANA));
        player->SetPower(POWER_MANA, newMana);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Harvest of Druzzil — restored {} mana.", manaGain);
        return true;
    }

    case AA_MAG_MANABURN:
    // Activate: spend 50% current mana; deal 500% of mana spent as arcane dmg. 10 min CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 600000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Manaburn on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        if (player->GetMaxPower(POWER_MANA) == 0)
        {
            handler->SendSysMessage("|cffff0000[AA]|r You have no mana resource.");
            return true;
        }
        int32 manaSpent = player->GetPower(POWER_MANA) / 2;
        if (manaSpent < 1)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Not enough mana.");
            return true;
        }
        uint32 dmg = (uint32)(manaSpent * 5);
        player->SetPower(POWER_MANA, player->GetPower(POWER_MANA) - manaSpent);
        Unit::DealDamage(player, tgt, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_ARCANE, nullptr, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Manaburn — spent {} mana, dealt {} arcane.", manaSpent, dmg);
        return true;
    }

    // =======================================================================
    // WARLOCK
    // =======================================================================

    case AA_WRL_FEARSTORM:
    // Activate: AoE fear all enemies within 10 yards. 3min CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 180000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Fearstorm on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        uint8 count = 0;
        for (Unit* u : NearbyEnemies(player, 10.0f))
        {
            player->CastSpell(u, 10890, true);  // Psychic Scream R4: 8s fear
            ++count;
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Fearstorm — {} target(s) feared.", count);
        return true;
    }

    case AA_WRL_LIFEBURN:
    // Activate: deal shadow dmg equal to 100% of current HP to target. 10 min CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 600000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Lifeburn on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        uint32 dmg = player->GetHealth();
        Unit::DealDamage(player, tgt, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Lifeburn — unleashed {} shadow damage!", dmg);
        return true;
    }

    case AA_WRL_LEECH_TOUCH:
    // Activate: deal 15/25/40% target HP as shadow; self-heal same. 5/4/3 min CD. Melee range.
    {
        static const uint32 cdMs[] = { 0, 300000, 240000, 180000 };
        uint32 cd = cdMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Leech Touch on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        if (player->GetDistance(tgt) > 5.0f)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Target must be in melee range.");
            return true;
        }
        static const float pct[] = { 0.0f, 0.15f, 0.25f, 0.40f };
        uint32 dmg = std::max(1u, (uint32)(tgt->GetHealth() * pct[rank]));
        Unit::DealDamage(player, tgt, dmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
        player->ModifyHealth((int32)dmg);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Leech Touch — drained/healed {}.", dmg);
        return true;
    }

    // =======================================================================
    // STUBS — complex scripted effects deferred to later phase
    // =======================================================================

    case AA_HUN_VOLLEY_BURST:           // 5207 — summon + AoE cast system needed
    case AA_HUN_SCOUT_OF_THE_WILD:      // 5218 — guardian spawn system needed
    case AA_ROG_ASSASSINS_MARK:         // 5315 — custom debuff aura system needed
    case AA_PRI_CHANNELING_DIVINE:      // 5403 — spell proc counting hook needed
    case AA_PRI_FORCEFUL_REJUVENATION:  // 5404 — CD reset loop implementation needed
    case AA_PRI_YAULP:                  // 5405 — toggle aura + mana regen hook needed
    case AA_PRI_CELESTIAL_HAMMER:       // 5406 — SP-based multi-hit spell delivery needed
    case AA_PRI_CELESTIAL_REGEN:        // 5407 — custom HoT aura delivery needed
    case AA_PRI_QUICK_BUFF:             // 5409 — cast-time intercept hook needed
    case AA_PRI_DIVINE_ARBITRATION:     // 5418 — multi-pet HP equalization needed
    case AA_PRI_CELESTIAL_BARRIER:      // 5420 — absorb aura delivery needed
    case AA_PRI_BESTOW_DIVINE_AURA:     // 5421 — target invulnerability aura needed
    case AA_PRI_SANCTIFICATION:         // 5424 — ground aura / DynObject system needed
    case AA_PRI_WAKE_OF_TRANQUILITY:    // 5426 — aggro radius modification needed
    case AA_MAG_FRENZIED_BURNOUT:       // 5735 — Water Elemental frenzy state needed
    case AA_MAG_MEND_COMPANION:         // 5736 — pet full-heal command needed
    case AA_WRL_WAKE_THE_DEAD:          // 5826 — slain demon re-summon needed
    case AA_WRL_DIRE_CHARM:             // 5829 — demon charm system needed
        handler->SendSysMessage("|cffff8c00[AA]|r This ability is not yet fully implemented.");
        return true;

    default:
        handler->SendSysMessage("|cffff0000[AA]|r That ability does not have an activate effect or is not recognized.");
        return true;
    }
}
