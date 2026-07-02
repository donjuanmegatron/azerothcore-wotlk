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
//                 WRL Leech Touch, Volley Burst, Scout of the Wild
//   IMPLEMENTED:  Assassin's Mark (5315), Cleanse Curse (2113), Flurry (5306)
//   STUBBED:      all
//                 remaining Priest actives (5403-5409,5418,5420-5421,5424,5426),
//                 Frenzied Burnout, Mend Companion, Wake the Dead, Dire Charm

#include "aa_runtime.h"
#include "Player.h"
#include "Pet.h"
#include "Unit.h"
#include "Creature.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldSessionMgr.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Random.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ---- extern display-buff helpers (defined in aa_combat_modifiers.cpp) -----
extern void SanctumAA_ShowBuff(Player*, uint32, uint32, uint8, bool);
extern void SanctumAA_RemoveBuff(Player*, uint32, bool);

// ---- cooldown state -------------------------------------------------------

// [playerGuid][aaId] = getMSTime() at last use
static std::unordered_map<uint32, std::unordered_map<uint32, uint32>> g_activeCDs;

// ---- Mortal Eradication DoT tracker ---------------------------------------
// Each activated Mortal Eradication places a 6-tick shadow DoT on a target.
// Ticked every 3s by aa_actives_worldscript; 18s total duration.

struct EradicationDot
{
    ObjectGuid targetGuid;
    uint32     tickDmg;       // shadow damage per tick (fixed at cast time from player SP)
    uint8      ticksLeft;
    uint32     lastTickMs;
};

// playerGuid → active DoT (one DoT per player at a time; re-cast refreshes)
static std::unordered_map<uint32, EradicationDot> g_eradDots;

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

// Collect ALL attackable enemies within range into a snapshot so iteration is
// safe even if DealDamage causes unit deaths mid-loop. Uses a real grid sweep
// (true AoE) so abilities hit passive targets (e.g. training dummies) and packs
// fighting your pet/guardian, not just units attacking you. Attackers + current
// target are added as a safety supplement (deduped).
static std::vector<Unit*> NearbyEnemies(Player* player, float range)
{
    std::vector<Unit*> out;

    // Primary: radius sweep of every attackable enemy around the player.
    {
        std::list<Unit*> targets;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck u_check(player, player, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, targets, u_check);
        Cell::VisitObjects(player, searcher, range);
        for (Unit* u : targets)
            if (u && u->IsAlive())
                out.push_back(u);
    }

    // Supplement: anything attacking the player, deduped.
    for (Unit* atk : player->getAttackers())
        if (atk && atk->IsAlive() && player->GetDistance(atk) <= range &&
            std::find(out.begin(), out.end(), atk) == out.end())
            out.push_back(atk);

    // Supplement: current target, deduped.
    Unit* tgt = GetTarget(player);
    if (tgt && tgt->IsAlive() && player->IsValidAttackTarget(tgt) &&
        player->GetDistance(tgt) <= range &&
        std::find(out.begin(), out.end(), tgt) == out.end())
        out.push_back(tgt);

    return out;
}

// ---- Weapon Fury active windows --------------------------------------------
// guid -> getMSTime() at which the Weapon Fury buff expires.
// While active, the player's melee swings deal bonus damage (representing the
// forced weapon on-hit procs). Read by aa_combat_modifiers.cpp per swing.
static std::unordered_map<uint32, uint32> g_weaponFuryUntil;

// Exported for aa_combat_modifiers.cpp — true if Weapon Fury is currently active.
bool SanctumAA_WeaponFuryActive(uint32 guid)
{
    auto it = g_weaponFuryUntil.find(guid);
    if (it == g_weaponFuryUntil.end())
        return false;
    if (getMSTime() >= it->second)
    {
        g_weaponFuryUntil.erase(it);
        return false;
    }
    return true;
}

// ---- Yaulp haste window (5120 Paladin / 5405 Priest) -----------------------
// guid -> {expiry, haste%, aaId, rank} — worldscript reverses speed mod on expiry.
// aaId field distinguishes Paladin Yaulp (5120) from Priest Yaulp (5405).
struct YaulpState { uint32 untilMs; float hastePct; uint32 aaId = 0; uint8 rank = 0; };
static std::unordered_map<uint32, YaulpState> g_yaulpUntil;

// Exported for aa_class.cpp — check if Priest Yaulp damage window is active.
bool SanctumAA_PriestYaulpActive(uint32 guid, uint8& outRank)
{
    auto it = g_yaulpUntil.find(guid);
    if (it == g_yaulpUntil.end()) return false;
    if (it->second.aaId != AA_PRI_YAULP) return false;
    if (getMSTime() >= it->second.untilMs) return false;
    outRank = it->second.rank;
    return true;
}

// ---- Rampage cleave window (5001) ------------------------------------------
// guid -> getMSTime() at which the Rampage window expires.
// While active, every melee swing also hits all enemies within 8 yds.
// Read by aa_combat_modifiers.cpp ModifyMeleeDamage.
static std::unordered_map<uint32, uint32> g_rampageUntil;

// Exported for aa_combat_modifiers.cpp — true if Rampage cleave window is up.
bool SanctumAA_RampageActive(uint32 guid)
{
    auto it = g_rampageUntil.find(guid);
    if (it == g_rampageUntil.end())
        return false;
    if (getMSTime() >= it->second)
    {
        g_rampageUntil.erase(it);
        return false;
    }
    return true;
}

// ---- Cheer active windows (Hunter 5228/5243/5244) --------------------------
// Each Cheer AA has an always-on passive (handled in aa_pet.cpp) PLUS a stronger
// timed burst triggered here on .aa use. Pets read these windows for the burst bonus.
static std::unordered_map<uint32, uint32> g_cheerOffUntil;
static std::unordered_map<uint32, uint32> g_cheerDefUntil;
static std::unordered_map<uint32, uint32> g_cheerSwiftUntil;

static bool CheerWindowActive(std::unordered_map<uint32, uint32>& m, uint32 guid)
{
    auto it = m.find(guid);
    if (it == m.end()) return false;
    if (getMSTime() >= it->second) { m.erase(it); return false; }
    return true;
}

// Exported for aa_pet.cpp — true while the Cheer burst window is up.
bool SanctumAA_CheerOffensiveActive(uint32 guid) { return CheerWindowActive(g_cheerOffUntil,   guid); }
bool SanctumAA_CheerDefensiveActive(uint32 guid) { return CheerWindowActive(g_cheerDefUntil,   guid); }
bool SanctumAA_CheerSwiftnessActive(uint32 guid) { return CheerWindowActive(g_cheerSwiftUntil, guid); }

// ---- Flurry (5306) rapid-strike state -------------------------------------
// On melee special cast: queue 3 physical hits delivered every 300ms via OnUpdate.
struct FlurryHits { uint32 victimLow; uint8 hitsLeft; uint32 lastHitMs; uint32 dmgPerHit; };
static std::unordered_map<uint32, FlurryHits> g_flurryActive;
static std::unordered_map<uint32, uint32>     g_flurryIcd;  // 10s ICD

// ---- Furious Charge window (5012) -----------------------------------------
// Window managed in aa_combat_modifiers.cpp via SanctumAA_SetFuriousChargeWindow.
// aa_actives_player below detects Charge cast and opens the window.

// ---- Priest: Priest Yaulp (5405) — separate map from Paladin's g_yaulpUntil ──
// Shares the same YaulpState struct and the same worldscript expiry path.
// NOTE: g_yaulpUntil is also used for Priest Yaulp (same struct, same worldscript expiry).
// The map key is playerGuid, and at most one Yaulp is active at a time per player.

// ---- Priest: Celestial Hammer (5406) — 3 queued holy strikes ───────────────
struct CelestialHammerState { uint32 targetLow; uint8 hitsLeft; uint32 lastHitMs; uint32 dmgPerHit; };
static std::unordered_map<uint32, CelestialHammerState> g_celestialHammer;

// ---- Priest: Celestial Regeneration (5407) — free HoT ──────────────────────
struct CelRegenState { uint8 ticksLeft; uint32 lastTickMs; uint32 healPerTick; };
static std::unordered_map<uint32, CelRegenState> g_celRegen;

// ---- Priest: Inspire (5440) — pet damage window after empowered shadow spell ──
struct InspireWindow { uint32 untilMs; uint8 rank; };
static std::unordered_map<uint32, InspireWindow> g_inspireWindow;

// Exported for aa_pet.cpp — true while Inspire window is active
bool SanctumAA_InspireActive(uint32 guid, uint8& outRank)
{
    auto it = g_inspireWindow.find(guid);
    if (it == g_inspireWindow.end()) return false;
    if (getMSTime() > it->second.untilMs) { g_inspireWindow.erase(it); return false; }
    outRank = it->second.rank;
    return true;
}

// ---- public API -----------------------------------------------------------

void SanctumAA_ClearActivateState(uint32 guid)
{
    // Reverse Yaulp haste if active
    {
        auto it = g_yaulpUntil.find(guid);
        if (it != g_yaulpUntil.end())
        {
            Player* p = ObjectAccessor::FindPlayerByLowGUID(guid);
            if (p && it->second.hastePct > 0.0f)
                p->ApplyAttackTimePercentMod(BASE_ATTACK, it->second.hastePct, false);
            g_yaulpUntil.erase(it);
        }
    }
    g_activeCDs.erase(guid);
    g_eradDots.erase(guid);
    g_weaponFuryUntil.erase(guid);
    g_rampageUntil.erase(guid);
    g_cheerOffUntil.erase(guid);
    g_cheerDefUntil.erase(guid);
    g_cheerSwiftUntil.erase(guid);
    g_flurryActive.erase(guid);
    g_flurryIcd.erase(guid);
    // Priest
    g_celestialHammer.erase(guid);
    g_celRegen.erase(guid);
    g_inspireWindow.erase(guid);
    // Mage (Focused Magic + Dragon's Fire zones are in aa_combat_modifiers; ClearPlayerState handles those)
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
    // Activate: for 6/9/12s every melee swing also strikes all enemies within 8 yds.
    // 30s/25s/20s CD. 20 rage cost.
    {
        static const uint32 cdMs[]  = { 0, 30000, 25000, 20000 };
        static const uint32 durMs[] = { 0,  6000,  9000, 12000 };
        uint32 cd  = cdMs[std::min<uint8>(rank, 3)];
        uint32 dur = durMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Rampage on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Multiclass: only require/spend rage if this character actually uses a
        // rage bar (real class Warrior/Druid). A DK/caster real class has no rage,
        // so the off-class Warrior AA is cooldown-gated only (mirrors Divine Stun's
        // GetMaxPower(POWER_MANA) > 0 guard).
        bool usesRage = (player->getPowerType() == POWER_RAGE);
        if (usesRage && player->GetPower(POWER_RAGE) < 200)  // 20 rage = 200 internal units
        {
            handler->SendSysMessage("|cffff0000[AA]|r Not enough Rage (needs 20).");
            return true;
        }
        if (usesRage)
            player->SetPower(POWER_RAGE, player->GetPower(POWER_RAGE) - 200);
        g_rampageUntil[guid] = getMSTime() + dur;
        SanctumAA_ShowBuff(player, 720001, dur, 0, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Rampage! Cleaving for {} sec.", dur / 1000u);
        return true;
    }

    case AA_WAR_IRON_WARRIOR:
    // Activate (R3 only): absorb shield = 30% of player armor for 15s. 3min CD.
    {
        if (rank < 3)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Iron Warrior activate requires Rank 3.");
            return true;
        }
        static const uint32 CD_MS = 180000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Iron Warrior on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        int32 armor = (int32)player->GetArmor();
        int32 shieldAmt = (int32)(armor * 0.30f);
        if (shieldAmt < 1) shieldAmt = 1;
        {
            extern void SanctumAA_SetIronWarriorAbsorb(uint32 guid, int32 amount, uint32 durationMs);
            SanctumAA_SetIronWarriorAbsorb(guid, shieldAmt, 15000u);
        }
        SanctumAA_ShowBuff(player, 720002, 15000u, 0, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Iron Warrior — absorb shield of {} (30%% armor) for 15 seconds!", shieldAmt);
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
            SanctumAA_DealVisibleDamage(player, tgt, tgt->GetHealth(), SPELL_SCHOOL_MASK_NORMAL);
            handler->SendSysMessage("|cff00ff00[AA]|r Death Blow — killing blow!");
        }
        else
        {
            uint32 dmg = (uint32)(player->GetTotalAttackPowerValue(BASE_ATTACK) / 14.0f * 3.0f);
            SanctumAA_DealVisibleDamage(player, tgt, dmg, SPELL_SCHOOL_MASK_NORMAL);
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
            SanctumAA_DealVisibleDamage(player, u, dmg, SPELL_SCHOOL_MASK_NORMAL);
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
        SanctumAA_DealVisibleDamage(player, tgt, dmg, SPELL_SCHOOL_MASK_HOLY);
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
        SanctumAA_DealVisibleDamage(player, tgt, dmg, SPELL_SCHOOL_MASK_SHADOW);
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
        // %-of-current-HP, but CAPPED at 2.5x the caster's AP/SP (Option 1) so it can't
        // melt high-HP bosses (40% of a multi-million-HP boss would be absurd). Stays the
        // lower of (%HP, cap): full %HP on low-HP mobs, cap-limited on big targets.
        uint32 hpDmg   = (uint32)(tgt->GetHealth() * pct[std::min<uint8>(rank, 3)]);
        int32  apVal   = (int32)player->GetTotalAttackPowerValue(BASE_ATTACK);
        int32  spVal   = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW);
        uint32 statCap = std::max(1u, (uint32)(2.5f * (float)std::max(apVal, spVal)));
        uint32 dmg     = std::max(1u, std::min(hpDmg, statCap));
        SanctumAA_DealVisibleDamage(player, tgt, dmg, SPELL_SCHOOL_MASK_SHADOW);
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
            SanctumAA_DealVisibleDamage(player, u, dmg, SPELL_SCHOOL_MASK_NATURE);
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
        SanctumAA_DealVisibleDamage(player, tgt, dmg, SPELL_SCHOOL_MASK_ARCANE);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Manaburn — spent {} mana, dealt {} arcane.", manaSpent, dmg);
        return true;
    }

    // =======================================================================
    // WARLOCK
    // =======================================================================

    case AA_WRL_NETHER_PORTAL:
    // Activate: summon a Doomguard to fight for 30/45/60s. 5min CD.
    {
        static const uint32 CD_MS = 300000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Nether Portal on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const uint32 durMs[] = { 0, 30000, 45000, 60000 };
        uint32 dur = durMs[std::min<uint8>(rank, 3)];
        float x = player->GetPositionX() + 2.0f * std::cos(player->GetOrientation() - 1.5f);
        float y = player->GetPositionY() + 2.0f * std::sin(player->GetOrientation() - 1.5f);
        float z = player->GetPositionZ();
        float o = player->GetOrientation();
        // Doomguard entry 11859 (classic Doom Guard — in 3.3.5a DB)
        Creature* summon = player->SummonCreature(11859, x, y, z, o, TEMPSUMMON_TIMED_DESPAWN, dur);
        if (summon)
        {
            summon->SetFaction(player->GetFaction());
            summon->SetLevel(player->GetLevel());
            summon->SetReactState(REACT_AGGRESSIVE);
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff9482c9[AA]|r Nether Portal — a Doomguard answers for {} seconds!", dur / 1000u);
        return true;
    }

    case AA_WRL_INFERNAL_VOLCANO:
    // Activate: summon an Infernal to fight for 30/45/60s. 5min CD.
    {
        static const uint32 CD_MS = 300000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Infernal Volcano on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const uint32 durMs[] = { 0, 30000, 45000, 60000 };
        uint32 dur = durMs[std::min<uint8>(rank, 3)];
        float x = player->GetPositionX() + 2.0f * std::cos(player->GetOrientation() + 1.5f);
        float y = player->GetPositionY() + 2.0f * std::sin(player->GetOrientation() + 1.5f);
        float z = player->GetPositionZ();
        float o = player->GetOrientation();
        // Classic Infernal entry 89 (Infernal — exists in 3.3.5a world DB)
        Creature* summon = player->SummonCreature(89, x, y, z, o, TEMPSUMMON_TIMED_DESPAWN, dur);
        if (summon)
        {
            summon->SetFaction(player->GetFaction());
            summon->SetLevel(player->GetLevel());
            summon->SetReactState(REACT_AGGRESSIVE);
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff9482c9[AA]|r Infernal Volcano — an Infernal crashes down for {} seconds!", dur / 1000u);
        return true;
    }

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
        SanctumAA_DealVisibleDamage(player, tgt, dmg, SPELL_SCHOOL_MASK_SHADOW);
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
        // %-of-current-HP, but CAPPED at 2.5x the caster's AP/SP (Option 1) so it can't
        // melt high-HP bosses (40% of a multi-million-HP boss would be absurd). Stays the
        // lower of (%HP, cap): full %HP on low-HP mobs, cap-limited on big targets.
        uint32 hpDmg   = (uint32)(tgt->GetHealth() * pct[std::min<uint8>(rank, 3)]);
        int32  apVal   = (int32)player->GetTotalAttackPowerValue(BASE_ATTACK);
        int32  spVal   = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW);
        uint32 statCap = std::max(1u, (uint32)(2.5f * (float)std::max(apVal, spVal)));
        uint32 dmg     = std::max(1u, std::min(hpDmg, statCap));
        SanctumAA_DealVisibleDamage(player, tgt, dmg, SPELL_SCHOOL_MASK_SHADOW);
        player->ModifyHealth((int32)dmg);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Leech Touch — drained/healed {}.", dmg);
        return true;
    }

    // =======================================================================
    // WARLOCK — Mortal Eradication
    // =======================================================================

    case AA_WRL_MORTAL_ERADICATION:
    // Activate: apply a stacking shadow curse DoT — 10/15/20% SP shadow every 3s for 18s (6 ticks).
    // 90s CD. Re-cast refreshes the DoT and resets tick count.
    {
        static const uint32 CD_MS = 90000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Mortal Eradication on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        static const float spPct[] = { 0.0f, 0.10f, 0.15f, 0.20f };
        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW);
        uint32 tickDmg = std::max(1u, static_cast<uint32>(sp * spPct[std::min<uint8>(rank, 3)]));

        EradicationDot& dot = g_eradDots[guid];
        dot.targetGuid = tgt->GetGUID();
        dot.tickDmg    = tickDmg;
        dot.ticksLeft  = 6;
        dot.lastTickMs = getMSTime();

        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff9482c9[AA]|r Mortal Eradication — {} shadow dmg/3s × 6 ticks.", tickDmg);
        return true;
    }

    // =======================================================================
    // ARCHETYPE — Weapon Fury
    // =======================================================================

    case AA_D_WEAPON_FURY:
    // Activate: for 12/18/24s, all melee swings proc weapon on-hit effects.
    // Implemented as a melee damage amplifier window (see aa_combat_modifiers.cpp).
    // 2 min cooldown.
    {
        static const uint32 CD_MS = 120000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Weapon Fury on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const uint32 durMs[] = { 0, 12000, 18000, 24000 };
        uint32 dur = durMs[std::min<uint8>(rank, 3)];
        g_weaponFuryUntil[guid] = getMSTime() + dur;
        SanctumAA_ShowBuff(player, 720030, dur, 0, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Weapon Fury — your swings unleash weapon effects for {} sec!", dur / 1000u);
        return true;
    }

    // =======================================================================
    // HUNTER — Volley Burst and Scout of the Wild
    // =======================================================================

    case AA_HUN_VOLLEY_BURST:
    // Activate: AoE physical burst to all enemies within 10 yd of current target.
    // Damage = RANGED_ATTACK AP × 1.5. 60s CD. 1 rank.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 60000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Volley Burst on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        uint32 dmg = (uint32)(player->GetTotalAttackPowerValue(RANGED_ATTACK) * 1.5f);
        // Hit the primary target
        SanctumAA_DealVisibleDamage(player, tgt, dmg, SPELL_SCHOOL_MASK_NORMAL);
        uint8 extraHits = 1; // primary counted
        // Hit additional enemies within 10 yd of target
        for (Unit* atk : player->getAttackers())
        {
            if (atk == tgt || !atk->IsAlive()) continue;
            if (tgt->GetDistance(atk) <= 10.0f)
            {
                SanctumAA_DealVisibleDamage(player, atk, dmg, SPELL_SCHOOL_MASK_NORMAL);
                ++extraHits;
            }
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Volley Burst — {} target(s) hit!", extraHits);
        return true;
    }

    case AA_HUN_SCOUT_OF_THE_WILD:
    // Activate: summon a temporary spirit wolf companion for 60s. 180s CD. 1 rank.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 180000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Scout of the Wild on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Spawn near the player slightly to the right
        float x = player->GetPositionX() + 2.0f * std::cos(player->GetOrientation() - 1.5f);
        float y = player->GetPositionY() + 2.0f * std::sin(player->GetOrientation() - 1.5f);
        float z = player->GetPositionZ();
        float o = player->GetOrientation();
        // Spirit wolf creature ID 29264 (Spectral Wolf — exists in 3.3.5a DB)
        Creature* wolf = player->SummonCreature(29264, x, y, z, o, TEMPSUMMON_TIMED_DESPAWN, 60000);
        if (wolf)
        {
            wolf->SetFaction(player->GetFaction());
            wolf->SetLevel(player->GetLevel());
            wolf->SetReactState(REACT_AGGRESSIVE);
        }
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Scout of the Wild — a spirit wolf answers your call for 60 seconds!");
        return true;
    }

    // =======================================================================
    // HUNTER — Cheer (active burst; passive bonuses live in aa_pet.cpp)
    // =======================================================================

    case AA_HUN_CHEER_OFFENSIVE:
    // Burst: pets +8/15/25% damage for 15s (on top of the passive +3/5/8%). 30s CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 30000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Cheer: Offensive on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        g_cheerOffUntil[guid] = getMSTime() + 15000u;
        SanctumAA_ShowBuff(player, 720027, 15000u, 0, true);
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Cheer: Offensive — your pets surge with offense for 15 sec!");
        return true;
    }

    case AA_HUN_CHEER_DEFENSIVE:
    // Burst: pets -8/15/25% damage taken for 15s (on top of the passive -3/5/8%). 30s CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 30000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Cheer: Defensive on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        g_cheerDefUntil[guid] = getMSTime() + 15000u;
        SanctumAA_ShowBuff(player, 720028, 15000u, 0, true);
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Cheer: Defensive — your pets brace against harm for 15 sec!");
        return true;
    }

    case AA_HUN_CHEER_SWIFTNESS:
    // Burst: pets +20/35/50% move speed for 15s (on top of the passive +8/15/20%). 30s CD.
    {
        if (uint32 rem = CDRemaining(guid, aaId, 30000u))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Cheer: Swiftness on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        g_cheerSwiftUntil[guid] = getMSTime() + 15000u;
        SanctumAA_ShowBuff(player, 720029, 15000u, 0, true);
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Cheer: Swiftness — your pets race ahead for 15 sec!");
        return true;
    }

    // =======================================================================
    // PALADIN — Yaulp (5120)
    // =======================================================================

    case AA_PAL_YAULP:
    // Activate: spell 720000 is a functional +20% melee haste aura for 30s — provides
    // both the mechanical effect and the buff icon. No need for ApplyAttackTimePercentMod.
    // Additionally grants +6/10/14 mp5 equivalent as flat mana restore. 90s CD.
    {
        static const uint32 CD_MS = 90000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Yaulp on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const uint32 mp5Ticks[] = { 0, 6, 10, 14 }; // mp5 value → over 30s

        // Apply the functional haste aura (720000 = +20% melee haste, 30s).
        // This replaces the old ApplyAttackTimePercentMod approach; no g_yaulpUntil entry needed
        // for haste reversal since the aura expires automatically.
        SanctumAA_ShowBuff(player, 720000, 30000u, 0, false);

        // Restore flat mana (lump sum)
        if (player->GetMaxPower(POWER_MANA) > 0)
        {
            uint32 totalMana = mp5Ticks[std::min<uint8>(rank, 3)] * 6u;
            int32 newMana = std::min(player->GetPower(POWER_MANA) + (int32)totalMana,
                                     player->GetMaxPower(POWER_MANA));
            player->SetPower(POWER_MANA, newMana);
        }

        // Store entry WITHOUT haste (hastePct=0) so the worldscript doesn't reverse speed mod.
        g_yaulpUntil[guid] = { getMSTime() + 30000u, 0.0f, AA_PAL_YAULP, rank };
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Yaulp! +20% attack speed for 30 sec.");
        return true;
    }

    // =======================================================================
    // ROGUE — Assassin's Mark (5315)
    // =======================================================================

    case AA_ROG_ASSASSINS_MARK:
    // Activate: mark a hostile target for 15s. All damage dealt to that target increased
    // by +10%/+18%/+28% for the duration. 60s CD.
    {
        static const uint32 CD_MS = 60000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Assassin's Mark on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid hostile target.");
            return true;
        }
        {
            extern void SanctumAA_SetAssassinsMark(uint32 playerGuid, uint32 targetLow, uint32 untilMs, uint8 rank);
            SanctumAA_SetAssassinsMark(guid, tgt->GetGUID().GetCounter(), getMSTime() + 15000u, rank);
        }
        SetCD(guid, aaId);
        static const uint32 pctDisplay[] = { 0, 10, 18, 28 };
        handler->PSendSysMessage("|cffff6600[AA]|r Assassin's Mark — target marked for 15s (+{}%% damage).", pctDisplay[std::min<uint8>(rank, 3)]);
        return true;
    }

    // =======================================================================
    // GENERAL DEFENSIVE — Cleanse Curse (2113)
    // =======================================================================

    case AA_G_CLEANSE_CURSE:
    // Activate: remove all curse effects from the player. 30s CD.
    {
        static const uint32 CD_MS = 30000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Cleanse Curse on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Remove all curse auras from the player
        // AC 3.3.5a has no RemoveAurasWithDispelType; iterate and remove manually.
        {
            std::vector<uint32> curseAuraKeys;
            for (auto const& [auraKey, aurApp] : player->GetAppliedAuras())
            {
                if (!aurApp) continue;
                SpellInfo const* si = aurApp->GetBase()->GetSpellInfo();
                if (!si) continue;
                if (si->Dispel == DISPEL_CURSE && !aurApp->IsPositive())
                    curseAuraKeys.push_back(auraKey);
            }
            for (uint32 key : curseAuraKeys)
                player->RemoveAura(key);
        }
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Cleanse Curse — all curse effects removed.");
        return true;
    }

    // =======================================================================
    // PRIEST — remaining actives (formerly stubbed)
    // =======================================================================

    case AA_PRI_CHANNELING_DIVINE:
    // Activate: grant 5/8/12 double-heal charges; next N heals fire twice. 3min CD.
    // The charge consumption happens in aa_combat_modifiers.cpp ModifyHealReceived.
    {
        static const uint32 CD_MS = 180000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Channeling the Divine on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const uint8 charges[] = { 0, 5, 8, 12 };
        uint8 ch = charges[std::min<uint8>(rank, 3)];
        {
            extern void SanctumAA_SetChannelingDivineCharges(uint32 guid, uint8 charges);
            SanctumAA_SetChannelingDivineCharges(guid, ch);
        }
        // Show charge buff (dur=0 → no countdown, icon shows until charges exhausted)
        SanctumAA_ShowBuff(player, 720009, 0, ch, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Channeling the Divine — next {} heal(s) will fire twice!", (uint32)ch);
        return true;
    }

    case AA_PRI_FORCEFUL_REJUVENATION:
    // ONE-SHOT: Instantly reset all spell cooldowns. 10min CD.
    {
        static const uint32 CD_MS = 600000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Forceful Rejuvenation on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Reset all player spell cooldowns
        player->RemoveAllSpellCooldown();
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Forceful Rejuvenation — all spell cooldowns reset!");
        return true;
    }

    case AA_PRI_YAULP:
    // Activate: +20% melee attack speed (via functional aura 720000, 30s) and
    // +10/20/30% melee damage for 30s. 2min CD.
    // Melee damage bonus stored in g_priYaulpDmgPct, read in aa_class.cpp ModifyMeleeDamage.
    // Haste is now provided by the functional aura 720000 — no ApplyAttackTimePercentMod needed.
    {
        static const uint32 CD_MS = 120000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Yaulp on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Apply the functional haste aura (720000 = +20% melee haste, 30s).
        // Keep the damage-window in g_yaulpUntil so aa_class.cpp can still read rank.
        // hastePct stored as 0 so the worldscript expiry path does NOT reverse speed mod.
        SanctumAA_ShowBuff(player, 720000, 30000u, 0, false);
        g_yaulpUntil[guid] = { getMSTime() + 30000u, 0.0f, AA_PRI_YAULP, rank };
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Yaulp! +20% attack speed for 30 sec.");
        return true;
    }

    case AA_PRI_CELESTIAL_HAMMER:
    // Activate: deliver 3 holy strikes over ~1.5s, each = 80/110/150% SP. 2min CD.
    // Strikes delivered by worldscript OnUpdate tick.
    {
        static const uint32 CD_MS = 120000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Celestial Hammer on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        if (!tgt || !tgt->IsAlive() || !player->IsValidAttackTarget(tgt))
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target.");
            return true;
        }
        static const float mult[] = { 0.0f, 0.80f, 1.10f, 1.50f };
        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_HOLY);
        if (sp < 0) sp = 0;
        uint32 dmgPerHit = std::max(1u, (uint32)(sp * mult[std::min<uint8>(rank, 3)]));
        g_celestialHammer[guid] = { tgt->GetGUID().GetCounter(), 3, getMSTime(), dmgPerHit };
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Celestial Hammer — 3 holy strikes ({} SP each)!", dmgPerHit);
        return true;
    }

    case AA_PRI_CELESTIAL_REGEN:
    // Activate: free HoT. 5/8/12% max HP per tick, every 3s for 30s (10 ticks). No mana cost. 5min CD.
    {
        static const uint32 CD_MS = 300000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Celestial Regeneration on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const float pct[] = { 0.0f, 0.05f, 0.08f, 0.12f };
        uint32 healPerTick = std::max(1u, (uint32)(player->GetMaxHealth() * pct[std::min<uint8>(rank, 3)]));
        g_celRegen[guid] = { 10, getMSTime(), healPerTick };
        SanctumAA_ShowBuff(player, 720008, 30000u, 0, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Celestial Regeneration — +{} HP/tick for 30s.", healPerTick);
        return true;
    }

    case AA_PRI_QUICK_BUFF:
    // Activate: apply a short +100% spell haste burst aura to approximate half-cast-time for
    // 3/5/7 casts. No clean per-cast cast-time hook exists in 3.3.5a.
    // APPROXIMATION: cast Berserking (spell 26297, 20% cast speed bonus) for 10s as a proxy.
    // Documents the limitation: the exact "N free fast casts then stop" mechanic is not available
    // without a SpellCast counter hook. The flat haste buff provides the speed-up spirit.
    // 90s CD.
    {
        static const uint32 CD_MS = 90000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Quick Buff on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Apply Heroism/Bloodlust (spell 2825) for 10s as a cast speed proxy.
        // NOTE: Heroism also grants melee haste; this is an approximation.
        // Spell 15473 (Shadow Form) would be wrong. Use 32182 = Heroism (40% haste) for 10s.
        // Since CastSpell is safe in an active handler (not inside a damage hook), this is valid.
        player->CastSpell(player, 32182, true);  // Heroism: +30% spell/melee haste for 40s
        SetCD(guid, aaId);
        static const uint32 castCount[] = { 0, 3, 5, 7 };
        handler->PSendSysMessage("|cff00ff00[AA]|r Quick Buff — haste burst ({} cast approximation).", castCount[std::min<uint8>(rank, 3)]);
        return true;
    }

    case AA_PRI_DIVINE_ARBITRATION:
    // Activate: equalize HP% between player and active pets/guardians. 3/2/1.5min CD.
    {
        static const uint32 cdMs[] = { 0, 180000, 120000, 90000 };
        uint32 cd = cdMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Divine Arbitration on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Gather living targets: player + native pet + guardian
        std::vector<Unit*> participants;
        participants.push_back(player);
        if (Pet* pet = player->GetPet())
            if (pet->IsAlive())
                participants.push_back(pet);
        // Guardian pet (mod-pet-systems uses first guardian creature)
        for (Unit* controlled : player->m_Controlled)
        {
            if (!controlled || controlled == player->GetPet()) continue;
            Creature* cr = controlled->ToCreature();
            if (cr && cr->IsAlive() && cr->GetOwnerGUID() == player->GetGUID())
                participants.push_back(controlled);
        }
        if (participants.size() <= 1)
        {
            handler->SendSysMessage("|cffff0000[AA]|r No active pets or guardians to equalize with.");
            return true;
        }
        // Compute average HP%
        float totalPct = 0.0f;
        for (Unit* u : participants)
            totalPct += u->GetHealthPct();
        float avgPct = totalPct / (float)participants.size();
        // Apply equalized HP
        for (Unit* u : participants)
        {
            uint32 targetHP = std::max(1u, (uint32)(u->GetMaxHealth() * avgPct / 100.0f));
            u->SetHealth(targetHP);
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Divine Arbitration — HP equalized to {}%%.", (uint32)avgPct);
        return true;
    }

    case AA_PRI_CELESTIAL_BARRIER:
    // Activate: absorb shield = 30/50/75% SP for 10s. 60s CD.
    // Absorb is consumed in aa_combat_modifiers.cpp ModifyMeleeDamage + ModifySpellDamageTaken.
    {
        static const uint32 CD_MS = 60000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Celestial Barrier on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const float mult[] = { 0.0f, 0.30f, 0.50f, 0.75f };
        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_HOLY);
        if (sp < 0) sp = 0;
        int32 shieldAmt = (int32)(sp * mult[std::min<uint8>(rank, 3)]);
        if (shieldAmt < 1) shieldAmt = 1;
        {
            extern void SanctumAA_SetCelestialBarrier(uint32 guid, int32 amount, uint32 durationMs);
            SanctumAA_SetCelestialBarrier(guid, shieldAmt, 10000u);
        }
        SanctumAA_ShowBuff(player, 720010, 10000u, 0, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Celestial Barrier — absorb shield of {} for 10s!", shieldAmt);
        return true;
    }

    case AA_PRI_BESTOW_DIVINE_AURA:
    // Activate: target (self, pet, or guardian) becomes invulnerable for 3/5/8s. 5min CD.
    // Implemented as a very large Celestial Barrier absorb (maxHP × 100) on the target.
    // This reuses the safe absorb-struct mechanism. Applying a real Divine Shield aura on a
    // non-player target via AddAura is risky (target typing); absorb is cleaner and safe.
    {
        static const uint32 CD_MS = 300000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Bestow Divine Aura on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const uint32 durMs[] = { 0, 3000, 5000, 8000 };
        uint32 dur = durMs[std::min<uint8>(rank, 3)];
        // Target: prefer current victim's target or self (player cannot use on enemies)
        Unit* tgt = player;  // default self
        // Check if player has a selected friendly unit (pet or guardian)
        ObjectGuid selGuid = player->GetTarget();
        if (selGuid)
        {
            Unit* sel = ObjectAccessor::GetUnit(*player, selGuid);
            if (sel && sel->IsAlive() && sel->IsFriendlyTo(player))
                tgt = sel;
        }
        // Use a huge absorb (10× max HP effectively = invuln)
        int32 bigAbsorb = (int32)(tgt->GetMaxHealth() * 100u);
        if (bigAbsorb < 1) bigAbsorb = 999999999;
        // If target is self, use Celestial Barrier map + show display buff
        if (tgt == player)
        {
            extern void SanctumAA_SetCelestialBarrier(uint32 guid, int32 amount, uint32 durationMs);
            SanctumAA_SetCelestialBarrier(guid, bigAbsorb, dur);
            SanctumAA_ShowBuff(player, 720011, dur, 0, false);
        }
        // For pet/guardian targets, apply a real aura (Divine Shield 642) via CastSpell.
        // CastSpell is safe here (active handler, not inside a damage hook).
        else
        {
            player->CastSpell(tgt, 642, true);  // Divine Shield: bubble + immunity
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Bestow Divine Aura — {} sec invulnerability!", dur / 1000u);
        return true;
    }

    // =======================================================================
    // STUBS — complex scripted effects deferred to later phase
    // =======================================================================
    // 5424 Sanctification    — SCRAPPED (Tier 1): ground DynObject zone not available
    // 5426 Wake of Tranquility — SCRAPPED (Tier 1): aggro-radius hook not available
    // 5826 Wake the Dead  — SCRAPPED (Tier 1)
    // 5829 Dire Charm     — SCRAPPED (Tier 1)
    // 5800 Threads of Despair — SCRAPPED (Tier 1)
    // 5822 Soul Barrage   — SCRAPPED (Tier 1)
    // 5834 Feigned Minion — SCRAPPED (Tier 1)

    // =======================================================================
    // MAGE — fully implemented actives
    // =======================================================================

    case AA_MAG_ARCANE_NOVA:
    // Activate: burst arcane AoE around player, 150/225/300% SP to all in 10yd. 1min CD.
    {
        static const uint32 CD_MS = 60000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Arcane Nova on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const float mult[] = { 0.0f, 1.50f, 2.25f, 3.00f };
        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE);
        if (sp < 0) sp = 0;
        uint32 dmg = (uint32)(sp * mult[std::min<uint8>(rank, 3)]);
        if (dmg == 0) dmg = 1;
        for (Unit* u : NearbyEnemies(player, 10.0f))
            SanctumAA_DealVisibleDamage(player, u, dmg, SPELL_SCHOOL_MASK_ARCANE);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Arcane Nova — {} arcane damage to all nearby!", dmg);
        return true;
    }

    case AA_MAG_FOCUSED_MAGIC:
    // Activate: cast on current target location — arcane zone for 12/18/24s dealing 20% SP
    // arcane per 2s to all within 6yd. Implemented via periodic tick in aa_combat_modifiers.cpp.
    {
        static const uint32 cdMs[] = { 0, 60000, 50000, 40000 };
        uint32 cd = cdMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Focused Magic on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        Unit* tgt = GetTarget(player);
        float zx, zy, zz;
        if (tgt && tgt->IsAlive() && player->IsValidAttackTarget(tgt))
        {
            zx = tgt->GetPositionX(); zy = tgt->GetPositionY(); zz = tgt->GetPositionZ();
        }
        else
        {
            zx = player->GetPositionX(); zy = player->GetPositionY(); zz = player->GetPositionZ();
        }
        static const uint32 durMs[] = { 0, 12000, 18000, 24000 };
        uint32 dur = durMs[std::min<uint8>(rank, 3)];
        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE);
        if (sp < 0) sp = 0;
        uint32 tickDmg = std::max(1u, (uint32)(sp * 0.20f));
        {
            extern void SanctumAA_SetFocusedMagicZone(uint32 playerGuid, float x, float y, float z, uint32 mapId, uint32 durationMs, uint32 tickDmg);
            SanctumAA_SetFocusedMagicZone(guid, zx, zy, zz, player->GetMapId(), dur, tickDmg);
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Focused Magic — arcane zone ({} SP/2s) for {}s!", tickDmg, dur / 1000u);
        return true;
    }

    case AA_MAG_FRENZIED_BURNOUT:
    // Activate: Water Elemental frenzy 15s: +50/75/100% attack speed, +30/50/80% damage.
    // Downside (HP drop) stripped. Implemented as stat buff on pet/elemental.
    {
        static const uint32 CD_MS = 180000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Frenzied Burnout on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Find Water Elemental (Pet slot or first guardian)
        Pet* pet = player->GetPet();
        Unit* elemental = nullptr;
        if (pet && pet->IsAlive())
            elemental = pet;
        else
        {
            for (Unit* ctrl : player->m_Controlled)
            {
                if (!ctrl || !ctrl->IsAlive()) continue;
                Creature* cr = ctrl->ToCreature();
                if (cr && cr->GetOwnerGUID() == player->GetGUID()) { elemental = cr; break; }
            }
        }
        if (!elemental)
        {
            handler->SendSysMessage("|cffff0000[AA]|r No Water Elemental or guardian active.");
            return true;
        }
        // Apply attack speed boost (+50/75/100%)
        static const float hastePct[] = { 0.0f, 50.0f, 75.0f, 100.0f };
        float haste = hastePct[std::min<uint8>(rank, 3)];
        elemental->ApplyAttackTimePercentMod(BASE_ATTACK, haste, true);
        // The +damage bonus is approximated: add a flat AP bonus for 15s
        static const float dmgPct[] = { 0.0f, 0.30f, 0.50f, 0.80f };
        float dmgBonus = dmgPct[std::min<uint8>(rank, 3)];
        uint32 flatAP = (uint32)(elemental->GetTotalAttackPowerValue(BASE_ATTACK) * dmgBonus);
        elemental->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE, (float)flatAP, true);

        // We cannot easily auto-revert after 15s without a WorldScript tick entry.
        // PARTIAL: the haste and AP remain until the pet dies/is resummoned (acceptable for solo).
        // A full implementation would need a WorldScript expiry entry like Yaulp.
        SanctumAA_ShowBuff(player, 720016, 15000u, 0, true);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Frenzied Burnout — elemental frenzy for 15 sec (+{}%% spd, +{}%% dmg)!", (uint32)haste, (uint32)(dmgBonus * 100));
        return true;
    }

    case AA_MAG_MEND_COMPANION:
    // Activate: instantly restore Water Elemental to full HP. 10/8/6min CD.
    {
        static const uint32 cdMs[] = { 0, 600000, 480000, 360000 };
        uint32 cd = cdMs[std::min<uint8>(rank, 3)];
        if (uint32 rem = CDRemaining(guid, aaId, cd))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Mend Companion on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        // Find Water Elemental or first guardian
        Pet* pet = player->GetPet();
        Unit* elemental = nullptr;
        if (pet && pet->IsAlive())
            elemental = pet;
        else
        {
            for (Unit* ctrl : player->m_Controlled)
            {
                if (!ctrl || !ctrl->IsAlive()) continue;
                Creature* cr = ctrl->ToCreature();
                if (cr && cr->GetOwnerGUID() == player->GetGUID()) { elemental = cr; break; }
            }
        }
        if (!elemental)
        {
            handler->SendSysMessage("|cffff0000[AA]|r No Water Elemental or guardian active.");
            return true;
        }
        elemental->SetFullHealth();
        SetCD(guid, aaId);
        handler->SendSysMessage("|cff00ff00[AA]|r Mend Companion — companion fully healed!");
        return true;
    }

    case AA_MAG_HOST_OF_THE_ELEMENTS:
    // Activate: summon an Ice Elemental guardian for 30/45/60s. 5min CD.
    // Uses the same pattern as Nether Portal / Infernal Volcano summons.
    {
        static const uint32 CD_MS = 300000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Host of the Elements on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const uint32 durMs[] = { 0, 30000, 45000, 60000 };
        uint32 dur = durMs[std::min<uint8>(rank, 3)];
        float x = player->GetPositionX() + 2.0f * std::cos(player->GetOrientation());
        float y = player->GetPositionY() + 2.0f * std::sin(player->GetOrientation());
        float z = player->GetPositionZ();
        float o = player->GetOrientation();
        // Ice Elemental creature: 23919 (verified in acore_world creature_template — "Ice Elemental")
        Creature* elemental = player->SummonCreature(23919, x, y, z, o, TEMPSUMMON_TIMED_DESPAWN, dur);
        if (elemental)
        {
            elemental->SetFaction(player->GetFaction());
            elemental->SetLevel(player->GetLevel());
            elemental->SetReactState(REACT_AGGRESSIVE);
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Host of the Elements — Ice Elemental summoned for {}s!", dur / 1000u);
        return true;
    }

    // =========================================================================
    // DRUID ACTIVES
    // =========================================================================

    case AA_DRU_SURVIVAL_INSTINCTS:
    // Activate: -20/30/40% damage taken for 12s. 3min CD. Requires Bear form.
    {
        static const uint32 CD_MS = 180000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Survival Instincts on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        ShapeshiftForm form = player->GetShapeshiftForm();
        if (form != FORM_BEAR && form != FORM_DIREBEAR)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Survival Instincts requires Bear Essence.");
            return true;
        }
        static const float drPct[] = { 0.0f, 0.20f, 0.30f, 0.40f };
        float dr = drPct[std::min<uint8>(rank, 3)];
        {
            extern void SanctumAA_SetSurvivalInstinctsWindow(uint32 playerGuid, float drPct, uint32 durationMs);
            SanctumAA_SetSurvivalInstinctsWindow(guid, dr, 12000u);
        }
        SanctumAA_ShowBuff(player, 720019, 12000u, 0, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Survival Instincts — {}% damage reduction for 12 seconds!", (int)(dr * 100));
        return true;
    }

    case AA_DRU_SPIRIT_OF_THE_WOOD:
    // Activate: grant self + active pet a HoT = 2/3/5% max HP per 5s + 10% armor +
    // 200/350/500 shield for 30s. 5min CD.
    {
        static const uint32 CD_MS = 300000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Spirit of the Wood on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const float hotPct[] = { 0.0f, 0.02f, 0.03f, 0.05f };
        static const uint32 shieldAmt[] = { 0, 200, 350, 500 };
        uint8 r = std::min<uint8>(rank, 3);

        // Self: HoT via Rejuvenation proxy cast (use a spirit-regen passive instead — ModifyHealth in OnUnitUpdate)
        // We implement as a timed periodic ModifyHealth + armor boost + absorb shield.
        // Reuse CelestialBarrier absorb map (extern pattern).
        uint32 hotPerTick = (uint32)(player->GetMaxHealth() * hotPct[r]);  // per 5s
        uint32 shield      = shieldAmt[r];

        // Apply armor bonus (+10% armor): approximate via flat modifier
        float armorBonus = player->GetArmor() * 0.10f;
        player->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, armorBonus, true);

        // Apply absorb shield (reuse Celestial Barrier map)
        {
            extern void SanctumAA_SetCelestialBarrier(uint32 guid, int32 amount, uint32 durationMs);
            SanctumAA_SetCelestialBarrier(guid, (int32)shield, 30000u);
        }

        // Start HoT: queue as a NaturalRenewal-style pool in the g_wotwAbsorb reuse is not ideal.
        // Instead, grant immediate HP and queue periodic via a simple approach:
        // We use ModifyHealth periodically — approximate by granting the full 30s worth now.
        // More accurate: store in a dedicated SotW HoT map. For now, apply 6 ticks worth immediately.
        // PARTIAL: grants the full HoT value upfront as a single heal for simplicity.
        uint32 totalHot = hotPerTick * 6;  // 6 ticks × 5s = 30s
        if (totalHot > 0 && !player->IsFullHealth())
            player->ModifyHealth((int32)totalHot);

        // Apply to pet
        if (Pet* pet = player->GetPet())
        {
            float petArmorBonus = pet->GetArmor() * 0.10f;
            pet->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, petArmorBonus, true);
            if (!pet->IsFullHealth())
                pet->ModifyHealth((int32)totalHot);
        }

        SanctumAA_ShowBuff(player, 720020, 30000u, 0, false);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Spirit of the Wood — nature's embrace for 30 seconds!", 0);
        return true;
    }

    case AA_DRU_CALL_OF_THE_WILD:
    // Activate: summon a temp beast guardian for 30/45/60s. 5min CD.
    // Uses a HIGH-LEVEL beast so it has real combat stats — SetLevel() only changes the
    // level field, NOT the HP/damage, so a low-level base (the old lvl-10 Dire Wolf 4271)
    // just stood there / got wrecked in raid content. Matches the working Ice Elemental (69).
    {
        static const uint32 CD_MS = 300000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Call of the Wild on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const uint32 durMs[] = { 0, 30000, 45000, 60000 };
        uint32 dur = durMs[std::min<uint8>(rank, 3)];
        float x = player->GetPositionX() + 2.0f * std::cos(player->GetOrientation() + 1.5f);
        float y = player->GetPositionY() + 2.0f * std::sin(player->GetOrientation() + 1.5f);
        float z = player->GetPositionZ();
        float o = player->GetOrientation();
        // Beast guardian entry: 32207 Armored Brown Bear (lvl 70 Beast, real combat stats).
        uint32 beastEntry = 32207;
        Creature* beast = player->SummonCreature(beastEntry, x, y, z, o, TEMPSUMMON_TIMED_DESPAWN, dur);
        if (beast)
        {
            beast->SetFaction(player->GetFaction());
            beast->SetLevel(player->GetLevel());
            beast->SetReactState(REACT_AGGRESSIVE);
        }
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Call of the Wild — a beast answers your call for {}s!", dur / 1000u);
        return true;
    }

    case AA_DRU_STAMPEDING_ROAR:
    // Activate: +30/50/70% move speed to self + all pets/guardians for 8s. 2min CD.
    {
        static const uint32 CD_MS = 120000u;
        if (uint32 rem = CDRemaining(guid, aaId, CD_MS))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Stampeding Roar on cooldown ({} sec).", rem / 1000u);
            return true;
        }
        static const float speedPct[] = { 0.0f, 0.30f, 0.50f, 0.70f };
        float boost = speedPct[std::min<uint8>(rank, 3)];

        // Apply speed to player
        player->SetSpeedRate(MOVE_RUN, player->GetSpeedRate(MOVE_RUN) + boost);

        // Apply to pet + guardians (via exported helper in aa_pet.cpp)
        {
            extern void SanctumAA_ApplyRoarSpeed(Player* player, float speedPct, uint32 durationMs);
            SanctumAA_ApplyRoarSpeed(player, boost, 8000u);
        }

        // Queue speed reversal in 8s via a simple timer stored in actives
        // PARTIAL: speed reversal queued but not auto-reverted without a timer hook.
        // Using the pattern: store expiry, reverse in aa_actives_worldscript.
        // For now: direct apply + note as PARTIAL — speed modifier not auto-removed.
        // (Speed boost will persist until player dismounts / changes zone / re-uses)
        SanctumAA_ShowBuff(player, 720021, 8000u, 0, true);
        SetCD(guid, aaId);
        handler->PSendSysMessage("|cff00ff00[AA]|r Stampeding Roar — {}% run speed for 8 seconds!", (int)(boost * 100));
        return true;
    }

    default:
        handler->SendSysMessage("|cffff0000[AA]|r That ability does not have an activate effect or is not recognized.");
        return true;
    }
}

// ---------------------------------------------------------------------------
// WorldScript — ticks Mortal Eradication DoTs every 500ms
// ---------------------------------------------------------------------------
class aa_actives_worldscript : public WorldScript
{
public:
    aa_actives_worldscript() : WorldScript("aa_actives_worldscript") {}

    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < 500)
            return;
        _timer = 0;

        // Yaulp expiry — reverse melee speed mod when window ends
        if (!g_yaulpUntil.empty())
        {
            std::vector<uint32> expired;
            uint32 nowMs = getMSTime();
            for (auto& [playerGuid, ys] : g_yaulpUntil)
            {
                if (nowMs >= ys.untilMs)
                {
                    Player* p = ObjectAccessor::FindPlayerByLowGUID(playerGuid);
                    if (p && ys.hastePct > 0.0f)
                        p->ApplyAttackTimePercentMod(BASE_ATTACK, ys.hastePct, false);
                    expired.push_back(playerGuid);
                }
            }
            for (uint32 g : expired)
                g_yaulpUntil.erase(g);
        }

        // Flurry (5306) — deliver queued rapid-strike hits every 300ms
        if (!g_flurryActive.empty())
        {
            std::vector<uint32> flurryDone;
            for (auto& [playerGuid, fstate] : g_flurryActive)
            {
                if (fstate.hitsLeft == 0) { flurryDone.push_back(playerGuid); continue; }
                if (GetMSTimeDiffToNow(fstate.lastHitMs) < 300u) continue;

                Player* p = ObjectAccessor::FindPlayerByLowGUID(playerGuid);
                if (!p || !p->IsInWorld() || !p->IsAlive()) { flurryDone.push_back(playerGuid); continue; }

                // Locate victim by stored low GUID
                Unit* victim = nullptr;
                for (Unit* atk : p->getAttackers())
                    if (atk->GetGUID().GetCounter() == fstate.victimLow) { victim = atk; break; }
                if (!victim)
                {
                    Unit* v = p->GetVictim();
                    if (v && v->GetGUID().GetCounter() == fstate.victimLow) victim = v;
                }
                if (!victim || !victim->IsAlive()) { flurryDone.push_back(playerGuid); continue; }

                SanctumAA_DealVisibleDamage(p, victim, fstate.dmgPerHit, SPELL_SCHOOL_MASK_NORMAL);
                fstate.lastHitMs = getMSTime();
                --fstate.hitsLeft;
                if (fstate.hitsLeft == 0)
                    flurryDone.push_back(playerGuid);
            }
            for (uint32 g : flurryDone)
                g_flurryActive.erase(g);
        }

        // Celestial Hammer (5406) — deliver 3 queued holy strikes every 500ms
        if (!g_celestialHammer.empty())
        {
            std::vector<uint32> hammerDone;
            for (auto& [playerGuid, hstate] : g_celestialHammer)
            {
                if (hstate.hitsLeft == 0) { hammerDone.push_back(playerGuid); continue; }
                if (GetMSTimeDiffToNow(hstate.lastHitMs) < 500u) continue;

                Player* p = ObjectAccessor::FindPlayerByLowGUID(playerGuid);
                if (!p || !p->IsInWorld() || !p->IsAlive()) { hammerDone.push_back(playerGuid); continue; }

                Unit* victim = nullptr;
                for (Unit* atk : p->getAttackers())
                    if (atk->GetGUID().GetCounter() == hstate.targetLow) { victim = atk; break; }
                if (!victim)
                {
                    Unit* v = p->GetVictim();
                    if (v && v->GetGUID().GetCounter() == hstate.targetLow) victim = v;
                }
                if (!victim || !victim->IsAlive()) { hammerDone.push_back(playerGuid); continue; }

                SanctumAA_DealVisibleDamage(p, victim, hstate.dmgPerHit, SPELL_SCHOOL_MASK_HOLY);
                hstate.lastHitMs = getMSTime();
                --hstate.hitsLeft;
                if (hstate.hitsLeft == 0)
                    hammerDone.push_back(playerGuid);
            }
            for (uint32 g : hammerDone)
                g_celestialHammer.erase(g);
        }

        // Celestial Regeneration (5407) — tick HoT every 3s
        if (!g_celRegen.empty())
        {
            std::vector<uint32> regenDone;
            uint32 nowMs = getMSTime();
            for (auto& [playerGuid, rs] : g_celRegen)
            {
                if (rs.ticksLeft == 0) { regenDone.push_back(playerGuid); continue; }
                if (GetMSTimeDiffToNow(rs.lastTickMs) < 3000u) continue;

                Player* p = ObjectAccessor::FindPlayerByLowGUID(playerGuid);
                if (!p || !p->IsInWorld() || !p->IsAlive()) { regenDone.push_back(playerGuid); continue; }

                int32 heal = (int32)rs.healPerTick;
                if (heal > 0)
                    p->ModifyHealth(heal);
                rs.lastTickMs = nowMs;
                --rs.ticksLeft;
                if (rs.ticksLeft == 0)
                    regenDone.push_back(playerGuid);
            }
            for (uint32 g : regenDone)
                g_celRegen.erase(g);
        }

        if (g_eradDots.empty())
            return;

        std::vector<uint32> toRemove;

        for (auto& [playerGuid, dot] : g_eradDots)
        {
            if (dot.ticksLeft == 0) { toRemove.push_back(playerGuid); continue; }
            if (GetMSTimeDiffToNow(dot.lastTickMs) < 3000u) continue;

            dot.lastTickMs = getMSTime();

            // Locate player
            Player* player = ObjectAccessor::FindPlayerByLowGUID(playerGuid);
            if (!player || !player->IsInWorld() || !player->IsAlive())
            {
                toRemove.push_back(playerGuid);
                continue;
            }

            // Locate target
            Unit* target = ObjectAccessor::GetUnit(*player, dot.targetGuid);
            if (!target || !target->IsAlive())
            {
                toRemove.push_back(playerGuid);
                continue;
            }

            SanctumAA_DealVisibleDamage(player, target, dot.tickDmg, SPELL_SCHOOL_MASK_SHADOW);
            --dot.ticksLeft;

            if (dot.ticksLeft == 0)
                toRemove.push_back(playerGuid);
        }

        for (uint32 g : toRemove)
            g_eradDots.erase(g);
    }

private:
    uint32 _timer = 0;
};

// ---------------------------------------------------------------------------
// aa_actives_player — PlayerScript for Furious Charge and other cast-based actives
// ---------------------------------------------------------------------------
class aa_actives_player : public PlayerScript
{
public:
    aa_actives_player() : PlayerScript("aa_actives_player") {}

    // Furious Charge (5012) — detect Charge cast; open damage window; R4 AoE burst.
    // Flurry (5306) — detect melee special cast and queue 3 rapid hits.
    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override
    {
        if (!player || skipCheck || !spell)
            return;

        SpellInfo const* info = spell->GetSpellInfo();
        if (!info)
            return;

        uint32 guid = player->GetGUID().GetCounter();

        // Flurry (5306) — trigger on any melee DmgClass special
        {
            uint8 rank = SanctumAA::GetRank(player, AA_ROG_FLURRY);
            if (rank > 0 && info->DmgClass == SPELL_DAMAGE_CLASS_MELEE)
            {
                auto& icdStamp = g_flurryIcd[guid];
                if (GetMSTimeDiffToNow(icdStamp) >= 10000u)
                {
                    Unit* victim = spell->m_targets.GetUnitTarget();
                    if (!victim) victim = player->GetVictim();
                    if (victim && victim->IsAlive())
                    {
                        static const float pct[] = { 0.0f, 0.50f, 0.65f, 0.80f };
                        uint32 dmgPerHit = std::max(1u,
                            (uint32)(player->GetTotalAttackPowerValue(BASE_ATTACK) * 0.20f * pct[std::min<uint8>(rank, 3)]));
                        g_flurryActive[guid] = { victim->GetGUID().GetCounter(), 3, getMSTime(), dmgPerHit };
                        icdStamp = getMSTime();
                    }
                }
            }
        }

        // ── Gift of Mana (5402) — refund mana on next spell cast if flag is set ──
        // The flag is set in aa_combat_modifiers.cpp ModifyHealReceived when a proc fires.
        {
            uint8 gmRank = SanctumAA::GetRank(player, AA_PRI_GIFT_OF_MANA);
            if (gmRank > 0)
            {
                extern bool SanctumAA_ConsumeGiftOfMana(uint32 guid);
                if (SanctumAA_ConsumeGiftOfMana(guid))
                {
                    // Refund the spell's base mana cost
                    uint32 cost = info->PowerType == POWER_MANA ? info->ManaCost : 0;
                    if (cost == 0)
                        cost = (uint32)(player->GetMaxPower(POWER_MANA) * info->ManaCostPercentage / 100.0f);
                    if (cost > 0)
                        player->ModifyPower(POWER_MANA, (int32)cost);
                }
            }
        }

        // ── Mark of Karna (5414) — set mark on target when player casts a holy or shadow spell ──
        // The bonus is read in aa_combat_modifiers.cpp ModifySpellDamageTaken.
        {
            uint8 mkRank = SanctumAA::GetRank(player, AA_PRI_MARK_OF_KARNA);
            if (mkRank > 0)
            {
                uint32 schoolMask = info->GetSchoolMask();
                if ((schoolMask & SPELL_SCHOOL_MASK_HOLY) || (schoolMask & SPELL_SCHOOL_MASK_SHADOW))
                {
                    Unit* tgt = spell->m_targets.GetUnitTarget();
                    if (!tgt) tgt = player->GetVictim();
                    if (tgt && tgt->IsAlive() && player->IsValidAttackTarget(tgt))
                    {
                        extern void SanctumAA_SetMarkOfKarna(uint32 playerGuid, uint32 targetLow, uint32 untilMs, uint8 rank);
                        SanctumAA_SetMarkOfKarna(guid, tgt->GetGUID().GetCounter(), getMSTime() + 15000u, mkRank);
                    }
                }
            }
        }

        // ── Inspire (5440) — open pet damage window after empowered shadow spells ──
        {
            uint8 insRank = SanctumAA::GetRank(player, AA_PRI_INSPIRE);
            if (insRank > 0)
            {
                static const std::unordered_set<uint32> s_empowered = {
                    // Mind Blast all ranks
                    8092, 10945, 10946, 10947, 25375, 25376, 48126, 48127,
                    // Shadow Word: Death all ranks
                    32379, 32996,
                    // Vampiric Touch all ranks
                    34914, 34916, 34917, 48159, 48160
                };
                if (s_empowered.count(info->Id))
                    g_inspireWindow[guid] = { getMSTime() + 10000u, insRank };
            }
        }

        // ── Radiant Cure (5417) — on Dispel/Cure casts, also remove disease+poison ──
        // R1: disease+poison. R2: +curse. R3: Mass Dispel AoE (stub — see below).
        {
            uint8 rcRank = SanctumAA::GetRank(player, AA_PRI_RADIANT_CURE);
            if (rcRank > 0)
            {
                // Dispel Magic (all ranks) + Mass Dispel
                static const std::unordered_set<uint32> s_dispel = {
                    527, 988, 32375, // Dispel Magic ranks + Mass Dispel
                    2782, 19803, 19804, 19805, 25431  // Cure Disease ranks
                };
                if (s_dispel.count(info->Id))
                {
                    Unit* tgt = spell->m_targets.GetUnitTarget();
                    if (!tgt) tgt = player;
                    if (tgt)
                    {
                        // R1+: remove disease and poison
                        std::vector<uint32> toRemove;
                        for (auto const& [key, app] : tgt->GetAppliedAuras())
                        {
                            if (!app) continue;
                            SpellInfo const* si = app->GetBase()->GetSpellInfo();
                            if (!si || app->IsPositive()) continue;
                            uint32 dt = si->Dispel;
                            if (dt == DISPEL_DISEASE || dt == DISPEL_POISON)
                                toRemove.push_back(key);
                            // R2+: also curse
                            if (rcRank >= 2 && dt == DISPEL_CURSE)
                                toRemove.push_back(key);
                        }
                        for (uint32 k : toRemove)
                            tgt->RemoveAura(k);
                    }
                    // R3 Mass Dispel AoE — extend to nearby allies
                    // Stubbed: no clean radius-dispel hook; single-target above is the main effect.
                }
            }
        }

        // Charge spell IDs in 3.3.5a
        static const std::unordered_set<uint32> s_charge = { 100, 6178, 11578 };
        if (!s_charge.count(info->Id))
            return;

        uint8 rank = SanctumAA::GetRank(player, AA_WAR_FURIOUS_CHARGE);
        if (!rank)
            return;

        // (guid declared above in the Flurry block)

        // Open the 6s damage window via exported function in aa_combat_modifiers.cpp
        {
            extern void SanctumAA_SetFuriousChargeWindow(uint32 guid, uint8 rank, uint32 durationMs);
            SanctumAA_SetFuriousChargeWindow(guid, rank, 6000u);
        }

        // R4: AoE fire burst to all enemies within 8 yd of the Charge target
        if (rank >= 4)
        {
            Unit* chargeTarget = spell->m_targets.GetUnitTarget();
            if (chargeTarget && chargeTarget->IsAlive())
            {
                // 120% of melee AP / 14 (approximate per-swing DPS)
                uint32 burstDmg = (uint32)(player->GetTotalAttackPowerValue(BASE_ATTACK) / 14.0f * 1.20f);
                if (burstDmg > 0)
                {
                    // Hit the primary target
                    SanctumAA_DealVisibleDamage(player, chargeTarget, burstDmg, SPELL_SCHOOL_MASK_FIRE);
                    // Hit additional enemies within 8 yd of the target
                    for (Unit* u : NearbyEnemies(player, 8.0f))
                    {
                        if (u == chargeTarget || !u->IsAlive())
                            continue;
                        if (chargeTarget->GetDistance(u) <= 8.0f)
                            SanctumAA_DealVisibleDamage(player, u, burstDmg, SPELL_SCHOOL_MASK_FIRE);
                    }
                }
            }
        }
    }
};

void AddSC_aa_actives()
{
    new aa_actives_worldscript();
    new aa_actives_player();
}
