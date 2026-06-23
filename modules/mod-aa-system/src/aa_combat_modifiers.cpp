// aa_combat_modifiers.cpp
//
// Sanctum AA System — Phase 2 combat modifier hooks.
//
// Implements passive and proc-based damage modifier AAs using UnitScript +
// PlayerScript hooks. All AAs here read their rank via SanctumAA::GetRank()
// defined in mod-aa-system.cpp; no direct DB access.
//
// IMPLEMENTED HERE (general + pet combat AAs):
//   2001  Double Strike       — 3/6/10% chance auto-attack hits twice (add 50% dmg)
//   2004  Killing Blow        — +10/20/30% dmg vs targets below 20% HP
//   2005  Vengeance           — burst hit taken (+20% maxHP) → +10/20/30% dmg for 8s
//   2006  Attention           — +2/4/6% dmg per attacker on player (cap 5)
//   2010  Thousand Cuts       — each hit adds stacking +1/2/3% dmg taken debuff (cap 5, 15s)
//   2011-2017 School Mastery  — +3/7/12% fire/frost/shadow/holy/nature/arcane/physical dmg
//   2018  Outburst            — +5/10/15% AoE spell dmg
//   2019  Berserker's Edge    — +20% dmg when attacker below 30% HP (one-shot rank)
//   2102  Thick Hide          — -1/2/3% physical dmg taken
//   2103  Warding             — -1/2/3% magical dmg taken
//   2104  Natural Renewal     — heal 25/60/110 HP per 5s (OnUnitUpdate tick)
//   2105  Reanimation         — below 20% HP: regen 2/4/6% missing HP per second
//   2107  Hardening           — each hit taken: +1% DR stack (max 5/7/10); resets on leave combat
//   2108  Bulwark             — single-hit cap at 35/30/25% max HP
//   2109  Hindsight           — hit > 30% max HP: absorb 30/60/100% of that hit for 6s (20s ICD)
//   2111  Recovery            — 5/10/15% of dmg taken healed back over 6s (1s ticks)
//   3001  Command             — +5/10/15% pet damage done
//   3002  Master's Bond       — +12/25/40% pet damage done (stacks with Command)
//   3103  Handler             — -5/10/15% pet damage taken
//   3203  Pack Leader         — pet attacks heal owner 2/4/6% of damage dealt
//   4203  Mortal Strike       — damage applies 30% healing reduction debuff (via WoW aura, ICD 5s)
//   4206  Twincast            — 5/10/15% chance spells fire twice at full effectiveness
//
// MOVED TO aa_archetype.cpp (single canonical implementation):
//   4102  Iron Resolve, 4104  Last Stand, 4204  Apex Predator
//
// MOVED TO mod-aa-system.cpp ApplyAAStat (handled as stat passives):
//   2003  Critical Mass       — +1/2/3% crit chance via ApplyRatingMod
//
// DEFERRED — Phase 3+ (require custom spell IDs or engine hooks not available):
//   2002  Precision           — crit damage bonus; no clean API in 3.3.5a without aura
//   4201  Bloodletting        — proc on crit; crit flag not exposed in damage hooks

#include "aa_runtime.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Unit.h"
#include "Creature.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Random.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// File-local runtime state — all keyed by player GUID counter (uint32 low)
// ---------------------------------------------------------------------------
namespace
{
    struct VengeanceState { uint32 untilMs = 0; uint8 rank = 0; };
    struct RecoveryState  { int32  pool    = 0; uint32 lastTickMs = 0; };
    struct RenewalState   { uint32 lastTickMs = 0; };
    struct CutsEntry      { uint8  stacks   = 0; uint32 expireMs = 0; };
    struct HardeningState { uint8  stacks   = 0; uint8  maxStacks = 5; };
    struct HindsightState { int32  absorb   = 0; uint32 expireMs  = 0; uint32 icdMs = 0; };

    // Retaliation (5011) — accumulated reflect pending dispatch
    struct RetaliationState { uint32 amount = 0; uint64 attackerGuid = 0; };

    // Iron Warrior absorb shield (5013) — active absorb + expiry
    struct IronWarriorAbsorb { int32 absorb = 0; uint32 expireMs = 0; };

    // Battle Endurance (5014) — once-per-combat trigger flag + active DR window
    //   used = has triggered this combat; shieldUntil = DR expiry (R2 only)
    struct BattleEnduranceState { bool used = false; uint32 shieldUntil = 0; };

    // Devastate debuff (5017) — attacker → victim → {stacks, expireMs, lastStackMs}
    struct DevastateEntry { uint8 stacks = 0; uint32 expireMs = 0; uint32 lastStackMs = 0; };

    // Furious Charge window (5012) — guid → window expiry
    struct FuriousChargeState { uint32 untilMs = 0; uint8 rank = 0; };

    std::unordered_map<uint32, VengeanceState> g_vengeance;
    std::unordered_map<uint32, RecoveryState>  g_recovery;
    std::unordered_map<uint32, RenewalState>   g_renewal;

    // Retaliation pending reflect: playerGuid → state
    std::unordered_map<uint32, RetaliationState> g_retaliation;

    // Iron Warrior absorb shield: playerGuid → state
    std::unordered_map<uint32, IronWarriorAbsorb> g_ironWarriorAbsorb;

    // Battle Endurance: playerGuid → state
    std::unordered_map<uint32, BattleEnduranceState> g_battleEndurance;

    // Furious Charge window: playerGuid → state
    std::unordered_map<uint32, FuriousChargeState> g_furiousCharge;

    // Devastate stacking debuff: attackerGuid → victimGuid → stack entry
    std::unordered_map<uint32, std::unordered_map<uint32, DevastateEntry>> g_devastate;

    // Recovery pool for Battle Endurance HoT: shares the existing RecoveryState map style
    // but keyed separately so it doesn't conflict
    struct BEHotState { int32 pool = 0; uint32 lastTickMs = 0; };
    std::unordered_map<uint32, BEHotState> g_beHot;

    // Thousand Cuts: attacker low → victim low → stack entry
    std::unordered_map<uint32, std::unordered_map<uint32, CutsEntry>> g_thousandCuts;

    // Improved Drain Life (5803): Drain Life ticks apply a shadow-DoT-vuln debuff.
    // Each tick: +1/2/3% more shadow DoT damage taken by that target, stacks to 5, 8s.
    // attackerGuid → victimGuid → {stacks, expireMs}
    struct DrainLifeEntry { uint8 stacks = 0; uint32 expireMs = 0; };
    std::unordered_map<uint32, std::unordered_map<uint32, DrainLifeEntry>> g_drainLife;

    // ICD tracker: guid → aaId → last proc timestamp (getMSTime())
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> g_icd;

    std::unordered_map<uint32, HardeningState> g_hardening;
    std::unordered_map<uint32, HindsightState> g_hindsight;
    std::unordered_map<uint32, uint32>         g_reanimTick;

    // ── Rogue: Puncture (5311) — stacking armor-shred debuff ─────────────────
    // attackerLow → victimLow → {stacks, expireMs}
    struct PunctureEntry { uint8 stacks = 0; uint32 expireMs = 0; };
    std::unordered_map<uint32, std::unordered_map<uint32, PunctureEntry>> g_puncture;

    // ── Rogue: Assassin's Mark (5315) — timed dmg bonus on a specific target ──
    struct AMark { uint32 targetLow; uint32 untilMs; uint8 rank; };
    std::unordered_map<uint32, AMark> g_assassinsMark;

    // ── Priest: Mark of Karna (5414) — like Assassin's Mark but triggered by holy/shadow spell cast ──
    struct KarnaMark { uint32 targetLow; uint32 untilMs; uint8 rank; };
    std::unordered_map<uint32, KarnaMark> g_markOfKarna;

    // ── Priest: Celestial Barrier (5420) — active absorb shield ──────────────
    struct CelestialBarrier { int32 absorb = 0; uint32 expireMs = 0; };
    std::unordered_map<uint32, CelestialBarrier> g_celestialBarrier;

    // ── Priest: Bestow Divine Aura (5421) — large absorb = effective invuln ──
    // Stored as CelestialBarrier in same map with a huge absorb value.

    // ── Priest: Twinheal (5401) — per-cast guard to prevent double-trigger ───
    // Value = getMSTime() stamp of last heal double; simple cooldown per cast.
    std::unordered_map<uint32, uint32> g_twinHealIcd;

    // ── Priest: Channeling the Divine (5403) — double-heal charges ───────────
    struct ChannelingState { uint8 charges = 0; };
    std::unordered_map<uint32, ChannelingState> g_channelingDivine;

    // ── Priest: Gift of Mana (5402) — "next spell free" flag ─────────────────
    std::unordered_map<uint32, bool> g_giftOfMana;

    // Recursion guard for Twincast
    bool g_inTwincast = false;

    // Recursion guard for Rampage cleave — prevents re-entrant loop if DealDamage
    // somehow feeds back into ModifyMeleeDamage (shouldn't happen, but cheap insurance).
    bool g_inRampageCleave = false;

    // ── Paladin: Judge (5101) —————————————————————————————————————————
    // Judgement cast opens a window: next N melee swings add +15% Holy bonus per swing.
    struct JudgeWindow { uint8 swingsLeft; uint32 untilMs; };
    std::unordered_map<uint32, JudgeWindow> g_judgeWindow;

    // ── Paladin: Improved Avenger's Shield (5110) — target debuff ————————
    // Targets hit by Avenger's Shield receive a damage-dealt-reduction debuff for 8s.
    struct AvengerDebuff { float dr; uint32 expireMs; };
    // playerGuid → victimGuid → debuff
    std::unordered_map<uint32, std::unordered_map<uint32, AvengerDebuff>> g_avengerDebuff;

    // ── Paladin: Sanctuary (5113) — pooled mini-heal from Blessing of Sanctuary procs ——
    // Pooled heals are applied in OnUnitUpdate, NOT inside the damage hook.
    std::unordered_map<uint32, int32> g_sanctuaryHealPool;

    // ── Paladin: Unyielding Light (5126) — post-Divine-Shield damage+heal window ——
    struct UnyieldingState { uint32 untilMs; uint8 rank; };
    std::unordered_map<uint32, UnyieldingState> g_unyieldingLight;

    // Track whether Divine Shield was active last tick (for edge-detect expiry).
    std::unordered_map<uint32, bool> g_divineShieldWasActive;

    // ── Paladin: Improved Flash of Light (5114) — Radiance stacks ———————
    // Each Flash of Light cast adds 1 stack (max 5); at 5 stacks the NEXT Flash heals more.
    std::unordered_map<uint32, uint8> g_radianceStacks;

    // ── Paladin: Crusader's Might (5100) — proc second CS hit ICD ————————
    std::unordered_map<uint32, uint32> g_crusaderMightIcd;

    // ── Sanctum Warden-pool heal (Sanctuary): boolean flag set by Blessing of Sanctuary trigger
    // Detection proxy: Blessing of Sanctuary aura IDs
    static const std::unordered_set<uint32> s_blessSanctuary = { 20911, 25899 };

    // -----------------------------------------------------------------------
    static inline Player* AsPlayer(Unit* u)
    {
        return (u && u->IsPlayer()) ? u->ToPlayer() : nullptr;
    }

    static bool CheckICD(uint32 guid, uint32 aaId, uint32 cdMs)
    {
        uint32& stamp = g_icd[guid][aaId];
        if (GetMSTimeDiffToNow(stamp) < cdMs)
            return false;
        stamp = getMSTime();
        return true;
    }

    template<typename T>
    static inline T Idx(uint8 rank) { return static_cast<T>(std::min<uint8>(rank, 3)); }

    static void ClearPlayerState(uint32 guid)
    {
        g_vengeance.erase(guid);
        g_recovery.erase(guid);
        g_renewal.erase(guid);
        g_icd.erase(guid);
        g_thousandCuts.erase(guid);
        for (auto& [ag, victimMap] : g_thousandCuts)
            victimMap.erase(guid);
        g_hardening.erase(guid);
        g_hindsight.erase(guid);
        g_reanimTick.erase(guid);
        g_retaliation.erase(guid);
        g_ironWarriorAbsorb.erase(guid);
        g_battleEndurance.erase(guid);
        g_furiousCharge.erase(guid);
        g_devastate.erase(guid);
        for (auto& [ag, victimMap] : g_devastate)
            victimMap.erase(guid);
        g_beHot.erase(guid);
        g_drainLife.erase(guid);
        for (auto& [ag, victimMap] : g_drainLife)
            victimMap.erase(guid);
        // Paladin / new state
        g_judgeWindow.erase(guid);
        g_avengerDebuff.erase(guid);
        for (auto& [ag, vm] : g_avengerDebuff) vm.erase(guid);
        g_sanctuaryHealPool.erase(guid);
        g_unyieldingLight.erase(guid);
        g_divineShieldWasActive.erase(guid);
        g_radianceStacks.erase(guid);
        g_crusaderMightIcd.erase(guid);
        // Rogue
        g_puncture.erase(guid);
        for (auto& [ag, vm] : g_puncture) vm.erase(guid);
        g_assassinsMark.erase(guid);
        // Priest
        g_markOfKarna.erase(guid);
        g_celestialBarrier.erase(guid);
        g_twinHealIcd.erase(guid);
        g_channelingDivine.erase(guid);
        g_giftOfMana.erase(guid);
    }
}

// ---------------------------------------------------------------------------
// Exported for aa_class.cpp — Devastate stack application
// ---------------------------------------------------------------------------
void SanctumAA_ApplyDevastateStack(uint32 attackerGuid, uint32 victimGuid, uint8 rank)
{
    auto& entry = g_devastate[attackerGuid][victimGuid];
    uint32 now  = getMSTime();

    // Clear stale stacks
    if (entry.stacks > 0 && now > entry.expireMs)
        entry = DevastateEntry{};

    // Stack cap scales per rank so R2 is a real upgrade, not a dead point sink:
    // R1 = 3 stacks (+6% dmg), R2 = 5 (+10%), R3 = 8 (+16%).
    static const uint8 maxStacksByRank[] = { 0, 3, 5, 8 };
    uint8 maxStacks = maxStacksByRank[(rank > 3) ? 3 : rank];

    // R3: at most one stack per second
    bool canStack = true;
    if (rank >= 3 && entry.stacks > 0 && GetMSTimeDiffToNow(entry.lastStackMs) < 1000u)
        canStack = false;

    if (canStack && entry.stacks < maxStacks)
    {
        entry.stacks++;
        entry.lastStackMs = now;
    }
    entry.expireMs = now + 15000u;
}

// ---------------------------------------------------------------------------
// Exported for aa_class.cpp — Judge window, Avenger debuff, Radiance stack
// ---------------------------------------------------------------------------
void SanctumAA_OpenJudgeWindow(uint32 guid, uint8 swings, uint32 durationMs)
{
    g_judgeWindow[guid] = { swings, getMSTime() + durationMs };
}

void SanctumAA_ApplyAvengerDebuff(uint32 playerGuid, uint32 targetGuid, float dr, uint32 durationMs)
{
    g_avengerDebuff[playerGuid][targetGuid] = { dr, getMSTime() + durationMs };
}

void SanctumAA_AddRadianceStack(uint32 guid)
{
    auto& stacks = g_radianceStacks[guid];
    if (stacks < 5) ++stacks;
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Assassin's Mark state management
// ---------------------------------------------------------------------------
void SanctumAA_SetAssassinsMark(uint32 playerGuid, uint32 targetLow, uint32 untilMs, uint8 rank)
{
    g_assassinsMark[playerGuid] = { targetLow, untilMs, rank };
}

bool SanctumAA_AssassinsMarkBonus(uint32 playerGuid, Unit* target, float& outPct)
{
    if (!target) return false;
    auto it = g_assassinsMark.find(playerGuid);
    if (it == g_assassinsMark.end()) return false;
    if (getMSTime() > it->second.untilMs) return false;
    if (target->GetGUID().GetCounter() != it->second.targetLow) return false;
    static const float b[] = { 0.0f, 0.10f, 0.18f, 0.28f };
    outPct = b[std::min<uint8>(it->second.rank, 3)];
    return true;
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Priest: Mark of Karna setter
// ---------------------------------------------------------------------------
void SanctumAA_SetMarkOfKarna(uint32 playerGuid, uint32 targetLow, uint32 untilMs, uint8 rank)
{
    g_markOfKarna[playerGuid] = { targetLow, untilMs, rank };
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Priest: Celestial Barrier absorb setter
// ---------------------------------------------------------------------------
void SanctumAA_SetCelestialBarrier(uint32 guid, int32 amount, uint32 durationMs)
{
    g_celestialBarrier[guid] = { amount, getMSTime() + durationMs };
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Priest: Channeling the Divine charge setter
// ---------------------------------------------------------------------------
void SanctumAA_SetChannelingDivineCharges(uint32 guid, uint8 charges)
{
    g_channelingDivine[guid].charges = charges;
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Priest: Gift of Mana flag setter
// (set by aa_actives_player OnPlayerSpellCast to grant the refund)
// ---------------------------------------------------------------------------
void SanctumAA_SetGiftOfMana(uint32 guid, bool val)
{
    g_giftOfMana[guid] = val;
}

bool SanctumAA_ConsumeGiftOfMana(uint32 guid)
{
    auto it = g_giftOfMana.find(guid);
    if (it == g_giftOfMana.end() || !it->second) return false;
    it->second = false;
    return true;
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Iron Warrior absorb shield activate
// ---------------------------------------------------------------------------
void SanctumAA_SetIronWarriorAbsorb(uint32 guid, int32 amount, uint32 durationMs)
{
    g_ironWarriorAbsorb[guid] = { amount, getMSTime() + durationMs };
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Furious Charge window set
// ---------------------------------------------------------------------------
void SanctumAA_SetFuriousChargeWindow(uint32 guid, uint8 rank, uint32 durationMs)
{
    g_furiousCharge[guid] = { getMSTime() + durationMs, rank };
}

// ---------------------------------------------------------------------------
// Visible AA damage — routes through the spell damage log so the player sees a
// floating yellow number, instead of silent server-side Unit::DealDamage.
// Attributed to a representative spell per school for the combat-log icon/name.
// ---------------------------------------------------------------------------
void SanctumAA_DealVisibleDamage(Player* attacker, Unit* victim, uint32 damage, uint32 schoolMask)
{
    if (!attacker || !victim || !victim->IsAlive() || damage == 0)
        return;

    uint32 attribSpell;
    switch (schoolMask)
    {
        case SPELL_SCHOOL_MASK_HOLY:   attribSpell = 48817; break; // Holy Wrath
        case SPELL_SCHOOL_MASK_FIRE:   attribSpell = 42833; break; // Fireball
        case SPELL_SCHOOL_MASK_NATURE: attribSpell = 49238; break; // Lightning Bolt
        case SPELL_SCHOOL_MASK_FROST:  attribSpell = 42842; break; // Frostbolt
        case SPELL_SCHOOL_MASK_SHADOW: attribSpell = 47632; break; // Death Coil
        case SPELL_SCHOOL_MASK_ARCANE: attribSpell = 42897; break; // Arcane Blast
        default:                       attribSpell = 1680;  break; // Whirlwind (physical)
    }

    SpellInfo const* si = sSpellMgr->GetSpellInfo(attribSpell);
    if (!si)
    {
        Unit::DealDamage(attacker, victim, damage, nullptr, DIRECT_DAMAGE,
                         (SpellSchoolMask)schoolMask, nullptr, false);
        return;
    }

    SpellNonMeleeDamage dmgLog(attacker, victim, si, (SpellSchoolMask)schoolMask);
    dmgLog.damage = damage;
    attacker->SendSpellNonMeleeDamageLog(&dmgLog);
    attacker->DealSpellDamage(&dmgLog, true);
}

// ---------------------------------------------------------------------------
// aa_combat_unit — UnitScript handling damage-modifier AAs
// ---------------------------------------------------------------------------
class aa_combat_unit : public UnitScript
{
public:
    aa_combat_unit() : UnitScript("aa_combat_unit", true,
    {
        UNITHOOK_ON_DAMAGE,
        UNITHOOK_MODIFY_MELEE_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
        UNITHOOK_MODIFY_HEAL_RECEIVED,
        UNITHOOK_ON_UNIT_UPDATE,
        UNITHOOK_ON_UNIT_DEATH,
        UNITHOOK_ON_UNIT_ENTER_EVADE_MODE,
    }) {}

    // -----------------------------------------------------------------------
    // OnDamage — fires for all damage types after calculation.
    //
    // Triggers state changes when the player is the VICTIM:
    //   Vengeance  — burst hit → queue damage buff
    //   Recovery   — any damage → queue heal-over-time
    //   Last Stand — HP drops below 25% → activate 20% DR (90s ICD)
    // -----------------------------------------------------------------------
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        Player* player = AsPlayer(victim);
        if (!player || damage == 0)
            return;

        uint32 guid = player->GetGUID().GetCounter();

        // Vengeance — burst hit triggers 8s damage buff
        {
            uint8 rank = SanctumAA::GetRank(player, AA_G_VENGEANCE);
            if (rank > 0 && damage >= player->GetMaxHealth() / 5u)
            {
                auto& v  = g_vengeance[guid];
                v.untilMs = getMSTime() + 8000u;
                v.rank    = rank;
            }
        }

        // Recovery — accumulate heal pool (healed back over 6s)
        {
            uint8 rank = SanctumAA::GetRank(player, AA_G_RECOVERY);
            if (rank > 0)
            {
                static const float pct[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                auto& r = g_recovery[guid];
                r.pool += (int32)(damage * pct[Idx<uint8>(rank)]);
                if (r.lastTickMs == 0)
                    r.lastTickMs = getMSTime();
            }
        }

        // Hardening — increment DR stack on each hit taken
        {
            uint8 rank = SanctumAA::GetRank(player, AA_G_HARDENING);
            if (rank > 0)
            {
                static const uint8 cap[] = { 0, 5, 7, 10 };
                auto& h     = g_hardening[guid];
                h.maxStacks = cap[Idx<uint8>(rank)];
                if (h.stacks < h.maxStacks)
                    h.stacks++;
            }
        }

        // Hindsight — hit > 30% max HP triggers absorb shield (20s ICD, 6s duration)
        {
            uint8 rank = SanctumAA::GetRank(player, AA_G_HINDSIGHT);
            if (rank > 0 && (float)damage >= (float)player->GetMaxHealth() * 0.30f)
            {
                auto& h = g_hindsight[guid];
                if (GetMSTimeDiffToNow(h.icdMs) >= 20000u)
                {
                    static const float shieldPct[] = { 0.0f, 0.30f, 0.60f, 1.00f };
                    uint32 triggerNow = getMSTime();
                    h.absorb   = (int32)(damage * shieldPct[Idx<uint8>(rank)]);
                    h.expireMs = triggerNow + 6000u;
                    h.icdMs    = triggerNow;
                }
            }
        }

    }

    // -----------------------------------------------------------------------
    // ModifyMeleeDamage — white auto-attacks ONLY (not abilities).
    //
    // Attacker-side (player): Double Strike, Killing Blow, Vengeance, Attention,
    //   Thousand Cuts, School Physical, Berserker's Edge, Mortal Strike debuff.
    // Attacker-side (pet): Command, Master's Bond, Pack Leader.
    // Victim-side (player): Thick Hide, Hardening, Bulwark, Hindsight, Iron Resolve, Last Stand.
    // Victim-side (pet): Handler.
    // -----------------------------------------------------------------------
    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (damage == 0)
            return;

        // ── ATTACKER IS PLAYER ──────────────────────────────────────────────
        if (Player* player = AsPlayer(attacker))
        {
            uint32 guid = player->GetGUID().GetCounter();

            // Weapon Fury (archetype active 4205) — while the activated window is up,
            // every melee swing deals +30% bonus damage (represents forced weapon procs).
            {
                extern bool SanctumAA_WeaponFuryActive(uint32 guid);
                if (SanctumAA::GetRank(player, AA_D_WEAPON_FURY) > 0 &&
                    SanctumAA_WeaponFuryActive(guid))
                {
                    damage += (uint32)(damage * 0.30f);
                }
            }

            // Rampage (5001) — cleave window: each white swing also strikes all other
            // enemies within 8 yds for the swing's damage. Routed through the spell
            // damage log (attributed to Whirlwind) so it shows VISIBLE yellow numbers
            // instead of silent server-side damage. Re-entrancy guarded by g_inRampageCleave.
            if (!g_inRampageCleave && target &&
                SanctumAA::GetRank(player, AA_WAR_RAMPAGE) > 0)
            {
                extern bool SanctumAA_RampageActive(uint32 guid);
                if (SanctumAA_RampageActive(guid))
                {
                    g_inRampageCleave = true;

                    SpellInfo const* splashSpell = sSpellMgr->GetSpellInfo(1680); // Whirlwind — visual attribution

                    auto cleaveHit = [&](Unit* u)
                    {
                        if (splashSpell)
                        {
                            SpellNonMeleeDamage dmgLog(player, u, splashSpell, SPELL_SCHOOL_MASK_NORMAL);
                            dmgLog.damage = damage;
                            player->SendSpellNonMeleeDamageLog(&dmgLog);
                            player->DealSpellDamage(&dmgLog, true);
                        }
                        else
                            Unit::DealDamage(player, u, damage, nullptr, DIRECT_DAMAGE,
                                             SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                    };

                    std::list<Unit*> nearList;
                    Acore::AnyUnfriendlyUnitInObjectRangeCheck uCheck(player, player, 8.0f);
                    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, uCheck);
                    Cell::VisitObjects(player, searcher, 8.0f);

                    for (Unit* u : nearList)
                    {
                        if (!u || !u->IsAlive() || u == target)
                            continue;
                        cleaveHit(u);
                    }
                    for (Unit* atk : player->getAttackers())
                    {
                        if (!atk || !atk->IsAlive() || atk == target)
                            continue;
                        if (player->GetDistance(atk) <= 8.0f &&
                            std::find(nearList.begin(), nearList.end(), atk) == nearList.end())
                            cleaveHit(atk);
                    }

                    g_inRampageCleave = false;
                }
            }

            // Judge (5101) — consume a swing from the active Judge window for +15% Holy bonus
            if (target)
            {
                auto it = g_judgeWindow.find(guid);
                if (it != g_judgeWindow.end() && it->second.swingsLeft > 0 &&
                    getMSTime() < it->second.untilMs)
                {
                    damage += (uint32)(damage * 0.15f);
                    --it->second.swingsLeft;
                    if (it->second.swingsLeft == 0)
                        g_judgeWindow.erase(it);
                }
            }

            // Unyielding Light (5126) — +dmg% while post-Divine-Shield window is active
            {
                auto it = g_unyieldingLight.find(guid);
                if (it != g_unyieldingLight.end() && getMSTime() < it->second.untilMs)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.18f, 0.30f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(it->second.rank)]);
                }
            }

            // Double Strike — 3/6/10% chance: add 50% extra
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_DOUBLE_STRIKE);
                if (rank > 0 && CheckICD(guid, AA_G_DOUBLE_STRIKE, 200))
                {
                    static const float chance[] = { 0.0f, 3.0f, 6.0f, 10.0f };
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        damage += damage / 2u;
                }
            }

            // Killing Blow — +10/20/30% vs targets below 20% HP
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_KILLING_BLOW);
                if (rank > 0 && target && target->GetHealthPct() < 20.0f)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Vengeance — +10/20/30% while buff window is active
            {
                auto it = g_vengeance.find(guid);
                if (it != g_vengeance.end() && getMSTime() < it->second.untilMs)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(it->second.rank)]);
                }
            }

            // Attention — +2/4/6% per enemy attacking player (cap 5)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_ATTENTION);
                if (rank > 0)
                {
                    size_t cnt = std::min<size_t>(player->getAttackers().size(), 5);
                    if (cnt > 0)
                    {
                        static const float perEnemy[] = { 0.0f, 0.02f, 0.04f, 0.06f };
                        damage += (uint32)(damage * perEnemy[Idx<uint8>(rank)] * cnt);
                    }
                }
            }

            // Thousand Cuts — increment debuff stack on victim, then apply bonus
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_THOUSAND_CUTS);
                if (rank > 0)
                {
                    uint32 victimGuid = target->GetGUID().GetCounter();
                    auto&  entry      = g_thousandCuts[guid][victimGuid];
                    uint32 now        = getMSTime();
                    if (entry.stacks > 0 && now > entry.expireMs)
                        entry = CutsEntry{};
                    if (entry.stacks < 5)
                        entry.stacks++;
                    entry.expireMs = now + 15000u;
                    static const float perStack[] = { 0.0f, 0.01f, 0.02f, 0.03f };
                    damage += (uint32)(damage * perStack[Idx<uint8>(rank)] * entry.stacks);
                }
            }

            // School Mastery: Physical — +3/7/12%
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_SCHOOL_PHYSICAL);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.03f, 0.07f, 0.12f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Berserker's Edge — +20% when attacker below 30% HP
            if (SanctumAA::Has(player, AA_G_BERSERKERS_EDGE) && player->GetHealthPct() < 30.0f)
                damage += damage / 5u;

            // Mortal Strike (DPS archetype) — healing reduction debuff deferred.
            // Applying spells/auras inside ModifyMeleeDamage causes re-entrant loops.
            // TODO: implement via OnUnitUpdate tick instead.

            // Furious Charge (5012) — +10/15/20% all damage window after Charge
            {
                auto it = g_furiousCharge.find(guid);
                if (it != g_furiousCharge.end() && getMSTime() < it->second.untilMs)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.15f, 0.20f, 0.20f };
                    uint8 r = std::min<uint8>(it->second.rank, 4);
                    damage += (uint32)(damage * bonus[r]);
                }
            }

            // Devastate debuff (5017) — read stacking damage bonus on target
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_IMPROVED_DEVASTATE);
                if (rank > 0)
                {
                    uint32 vGuid = target->GetGUID().GetCounter();
                    auto it = g_devastate.find(guid);
                    if (it != g_devastate.end())
                    {
                        auto jt = it->second.find(vGuid);
                        if (jt != it->second.end() && jt->second.stacks > 0)
                        {
                            uint32 now = getMSTime();
                            if (now <= jt->second.expireMs)
                                damage += (uint32)(damage * 0.02f * jt->second.stacks);
                        }
                    }
                }
            }

            // Puncture (5311) — stacking armor-shred: +1/2/3% damage per stack (cap 5)
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_PUNCTURE);
                if (rank > 0)
                {
                    uint32 victimGuid = target->GetGUID().GetCounter();
                    auto&  entry      = g_puncture[guid][victimGuid];
                    uint32 now        = getMSTime();
                    if (entry.stacks > 0 && now > entry.expireMs)
                        entry = PunctureEntry{};
                    if (entry.stacks < 5)
                        entry.stacks++;
                    entry.expireMs = now + 5000u;
                    static const float perStack[] = { 0.0f, 0.01f, 0.02f, 0.03f };
                    damage += (uint32)(damage * perStack[Idx<uint8>(rank)] * entry.stacks);
                }
            }

            // Assassin's Mark (5315) — melee bonus on marked target
            if (target)
            {
                float amBonus = 0.0f;
                if (SanctumAA_AssassinsMarkBonus(guid, target, amBonus) && amBonus > 0.0f)
                    damage += (uint32)(damage * amBonus);
            }

        } // end ATTACKER IS PLAYER

        // ── ATTACKER IS PET (owner is player) ──────────────────────────────
        if (!AsPlayer(attacker) && attacker)
        {
            if (Unit* petOwner = attacker->GetOwner())
            {
                if (Player* pOwner = petOwner->ToPlayer())
                {
                    // Command — +5/10/15% pet damage done
                    {
                        uint8 rank = SanctumAA::GetRank(pOwner, AA_P_COMMAND);
                        if (rank > 0)
                        {
                            static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                            damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                        }
                    }

                    // Master's Bond — +12/25/40% pet damage done
                    {
                        uint8 rank = SanctumAA::GetRank(pOwner, AA_P_MASTERS_BOND);
                        if (rank > 0)
                        {
                            static const float bonus[] = { 0.0f, 0.12f, 0.25f, 0.40f };
                            damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                        }
                    }

                    // Pack Leader — pet attacks heal owner 2/4/6% of damage dealt
                    {
                        uint8 rank = SanctumAA::GetRank(pOwner, AA_P_PACK_LEADER);
                        if (rank > 0 && damage > 0 && !pOwner->IsFullHealth())
                        {
                            static const float pct[] = { 0.0f, 0.02f, 0.04f, 0.06f };
                            int32 healAmt = (int32)(damage * pct[Idx<uint8>(rank)]);
                            if (healAmt > 0)
                                pOwner->ModifyHealth(healAmt);
                        }
                    }

                    // Inspire (5440) — +8/15/25% pet damage for 10s after empowered shadow spell
                    {
                        extern bool SanctumAA_InspireActive(uint32 guid, uint8& outRank);
                        uint8 insRank = 0;
                        if (SanctumAA_InspireActive(pOwner->GetGUID().GetCounter(), insRank))
                        {
                            static const float bonus[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                            damage += (uint32)(damage * bonus[Idx<uint8>(insRank)]);
                        }
                    }
                }
            }
        } // end ATTACKER IS PET

        // ── VICTIM IS PLAYER ────────────────────────────────────────────────
        if (Player* player = AsPlayer(target))
        {
            uint32 vGuid = player->GetGUID().GetCounter();

            // Thick Hide — -1/2/3% physical DR
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_THICK_HIDE);
                if (rank > 0)
                {
                    static const float dr[] = { 0.0f, 0.01f, 0.02f, 0.03f };
                    damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                }
            }

            // Hardening — -1% DR per stack
            {
                auto it = g_hardening.find(vGuid);
                if (it != g_hardening.end() && it->second.stacks > 0)
                    damage = (uint32)(damage * (1.0f - 0.01f * it->second.stacks));
            }

            // Bulwark — cap single hit at 35/30/25% max HP
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_BULWARK);
                if (rank > 0)
                {
                    static const float capPct[] = { 0.0f, 0.35f, 0.30f, 0.25f };
                    uint32 cap = (uint32)(player->GetMaxHealth() * capPct[Idx<uint8>(rank)]);
                    if (damage > cap)
                        damage = cap;
                }
            }

            // Hindsight — drain absorb shield before hit lands
            {
                auto it = g_hindsight.find(vGuid);
                if (it != g_hindsight.end() && it->second.absorb > 0)
                {
                    if (getMSTime() > it->second.expireMs)
                        it->second.absorb = 0;
                    else
                    {
                        uint32 absorbed = std::min((uint32)it->second.absorb, damage);
                        damage -= absorbed;
                        it->second.absorb -= (int32)absorbed;
                    }
                }
            }

            // Iron Warrior (5013) — DR scaling with missing HP; absorb shield
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_IRON_WARRIOR);
                if (rank > 0 && damage > 0)
                {
                    // Passive: DR = maxDR * (1 - currentHPpct/100)
                    static const float maxDR[] = { 0.0f, 0.10f, 0.15f, 0.20f };
                    float hpPct = player->GetHealthPct() / 100.0f;
                    float dr    = maxDR[Idx<uint8>(rank)] * (1.0f - hpPct);
                    if (dr > 0.0f)
                        damage = (uint32)(damage * (1.0f - dr));

                    // R3 absorb shield — drain before hit
                    auto it = g_ironWarriorAbsorb.find(vGuid);
                    if (it != g_ironWarriorAbsorb.end() && it->second.absorb > 0)
                    {
                        if (getMSTime() > it->second.expireMs)
                            it->second.absorb = 0;
                        else
                        {
                            uint32 absorbed = std::min((uint32)it->second.absorb, damage);
                            damage -= absorbed;
                            it->second.absorb -= (int32)absorbed;
                        }
                    }
                }
            }

            // Battle Endurance (5014) — intercept killing blow once per combat
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_BATTLE_ENDURANCE);
                if (rank > 0 && player->IsAlive() && damage >= player->GetHealth())
                {
                    auto& be = g_battleEndurance[vGuid];
                    if (!be.used)
                    {
                        be.used = true;
                        static const float survivePct[] = { 0.0f, 0.10f, 0.15f };
                        float sp = survivePct[std::min<uint8>(rank, 2)];
                        uint32 surviveHP = (uint32)(player->GetMaxHealth() * sp);
                        // Clamp damage so player survives at sp% HP
                        damage = player->GetHealth() > surviveHP ? (player->GetHealth() - surviveHP) : 0;

                        // Queue a HoT: 20% max HP over 4s (1s ticks)
                        auto& hot = g_beHot[vGuid];
                        hot.pool       = (int32)(player->GetMaxHealth() * 0.20f);
                        hot.lastTickMs = getMSTime();

                        // R2: grant -30% damage taken window for 4s
                        if (rank >= 2)
                            be.shieldUntil = getMSTime() + 4000u;
                    }
                }
            }

            // Battle Endurance R2 — -30% damage taken DR window
            {
                auto it = g_battleEndurance.find(vGuid);
                if (it != g_battleEndurance.end() && it->second.shieldUntil > 0)
                {
                    if (getMSTime() < it->second.shieldUntil)
                        damage = (uint32)(damage * 0.70f);
                    else
                        it->second.shieldUntil = 0;
                }
            }

            // Retaliation (5011) — accumulate reflect amount for worldscript dispatch
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_RETALIATION);
                if (rank > 0 && damage > 0 && attacker)
                {
                    static const float reflectPct[] = { 0.0f, 0.15f, 0.25f };
                    uint32 reflectAmt = (uint32)(damage * reflectPct[std::min<uint8>(rank, 2)]);
                    if (reflectAmt > 0)
                    {
                        auto& ret = g_retaliation[vGuid];
                        ret.amount      += reflectAmt;
                        ret.attackerGuid = attacker->GetGUID().GetRawValue();
                    }
                }
            }

            // Improved Avenger's Shield (5110) — read attacker debuff: reduce dmg dealt
            if (attacker)
            {
                uint32 atkGuid = attacker->GetGUID().GetCounter();
                // Check if this attacker has the Avenger debuff applied by THIS player
                auto pit = g_avengerDebuff.find(vGuid);
                if (pit != g_avengerDebuff.end())
                {
                    auto ait = pit->second.find(atkGuid);
                    if (ait != pit->second.end() && getMSTime() < ait->second.expireMs)
                    {
                        damage = (uint32)(damage * (1.0f - ait->second.dr));
                    }
                }
            }

            // Sanctuary (5113) — pool a small heal when Blessing of Sanctuary is active
            // (proxy: player has the Blessing of Sanctuary aura)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_SANCTUARY);
                if (rank > 0 && damage > 0)
                {
                    bool hasSanct = false;
                    for (uint32 id : s_blessSanctuary)
                        if (player->HasAura(id)) { hasSanct = true; break; }
                    if (hasSanct)
                    {
                        static const float pct[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                        int32 healAmt = (int32)(player->GetMaxHealth() * pct[Idx<uint8>(rank)]);
                        if (healAmt > 0)
                            g_sanctuaryHealPool[vGuid] += healAmt;
                    }
                }
            }

            // Unyielding Light (5126) — victim side: reduce damage during active window
            // (the window also buffs damage dealt, handled in attacker section above)
            {
                auto it = g_unyieldingLight.find(vGuid);
                if (it != g_unyieldingLight.end() && getMSTime() < it->second.untilMs)
                {
                    // Window is a damage bonus, not a DR — no reduction here (attacker side only).
                    // Nothing to do on victim side for melee DR.
                }
            }

            // Celestial Barrier (5420) — absorb shield consuming melee damage
            // Safe to use ModifyHealth here (not a damage call).
            {
                auto it = g_celestialBarrier.find(vGuid);
                if (it != g_celestialBarrier.end() && it->second.absorb > 0 &&
                    getMSTime() < it->second.expireMs)
                {
                    int32 absorbed = std::min((int32)damage, it->second.absorb);
                    it->second.absorb -= absorbed;
                    damage = (uint32)((int32)damage - absorbed);
                    if (damage == 0) damage = 0;
                    if (it->second.absorb <= 0)
                        g_celestialBarrier.erase(it);
                }
            }

            // Touch of the Divine (5423) — reflect 15/25/40% SP as holy to attacker on each melee hit
            // Uses SanctumAA_DealVisibleDamage which is safe (not re-entrant through ModifyMeleeDamage).
            if (attacker && attacker->IsAlive() && damage > 0)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PRI_TOUCH_OF_THE_DIVINE);
                if (rank > 0)
                {
                    static const float pct[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                    int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_HOLY);
                    if (sp < 0) sp = 0;
                    uint32 reflectDmg = (uint32)(sp * pct[Idx<uint8>(rank)]);
                    if (reflectDmg > 0)
                        SanctumAA_DealVisibleDamage(player, attacker, reflectDmg, SPELL_SCHOOL_MASK_HOLY);
                }
            }

        } // end VICTIM IS PLAYER

        // ── VICTIM IS PET (owner is player) ────────────────────────────────
        if (target && !AsPlayer(target))
        {
            if (Unit* petOwner = target->GetOwner())
            {
                if (Player* pOwner = petOwner->ToPlayer())
                {
                    // Handler — -5/10/15% pet damage taken
                    uint8 rank = SanctumAA::GetRank(pOwner, AA_P_HANDLER);
                    if (rank > 0)
                    {
                        static const float dr[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }
            }
        } // end VICTIM IS PET
    }

    // -----------------------------------------------------------------------
    // ModifySpellDamageTaken — all spell and ability damage (not white hits).
    // -----------------------------------------------------------------------
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage <= 0 || !spellInfo)
            return;

        uint32 schoolMask = spellInfo->GetSchoolMask();
        bool   isPhysical = (schoolMask & SPELL_SCHOOL_MASK_NORMAL) != 0;
        bool   isMagical  = !isPhysical;
        bool   isAoE      = spellInfo->IsTargetingArea();

        // ── ATTACKER IS PLAYER ──────────────────────────────────────────────
        if (Player* player = AsPlayer(attacker))
        {
            uint32 guid = player->GetGUID().GetCounter();

            // School Mastery
            {
                static const float bonus[] = { 0.0f, 0.03f, 0.07f, 0.12f };
                uint8 rank = 0;
                if      (schoolMask & SPELL_SCHOOL_MASK_FIRE)   rank = SanctumAA::GetRank(player, AA_G_SCHOOL_FIRE);
                else if (schoolMask & SPELL_SCHOOL_MASK_FROST)  rank = SanctumAA::GetRank(player, AA_G_SCHOOL_FROST);
                else if (schoolMask & SPELL_SCHOOL_MASK_SHADOW) rank = SanctumAA::GetRank(player, AA_G_SCHOOL_SHADOW);
                else if (schoolMask & SPELL_SCHOOL_MASK_HOLY)   rank = SanctumAA::GetRank(player, AA_G_SCHOOL_HOLY);
                else if (schoolMask & SPELL_SCHOOL_MASK_NATURE) rank = SanctumAA::GetRank(player, AA_G_SCHOOL_NATURE);
                else if (schoolMask & SPELL_SCHOOL_MASK_ARCANE) rank = SanctumAA::GetRank(player, AA_G_SCHOOL_ARCANE);
                else if (isPhysical)                            rank = SanctumAA::GetRank(player, AA_G_SCHOOL_PHYSICAL);
                if (rank > 0)
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
            }

            // Outburst — +5/10/15% AoE spell damage
            if (isAoE)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_OUTBURST);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Killing Blow — +10/20/30% vs targets below 20% HP
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_KILLING_BLOW);
                if (rank > 0 && target && target->GetHealthPct() < 20.0f)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Vengeance
            {
                auto it = g_vengeance.find(guid);
                if (it != g_vengeance.end() && getMSTime() < it->second.untilMs)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                    damage += (int32)(damage * bonus[Idx<uint8>(it->second.rank)]);
                }
            }

            // Attention
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_ATTENTION);
                if (rank > 0)
                {
                    size_t cnt = std::min<size_t>(player->getAttackers().size(), 5);
                    if (cnt > 0)
                    {
                        static const float perEnemy[] = { 0.0f, 0.02f, 0.04f, 0.06f };
                        damage += (int32)(damage * perEnemy[Idx<uint8>(rank)] * cnt);
                    }
                }
            }

            // Thousand Cuts
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_THOUSAND_CUTS);
                if (rank > 0)
                {
                    uint32 victimGuid = target->GetGUID().GetCounter();
                    auto&  entry      = g_thousandCuts[guid][victimGuid];
                    uint32 now        = getMSTime();
                    if (entry.stacks > 0 && now > entry.expireMs)
                        entry = CutsEntry{};
                    if (entry.stacks < 5)
                        entry.stacks++;
                    entry.expireMs = now + 15000u;
                    static const float perStack[] = { 0.0f, 0.01f, 0.02f, 0.03f };
                    damage += (int32)(damage * perStack[Idx<uint8>(rank)] * entry.stacks);
                }
            }

            // Berserker's Edge
            if (SanctumAA::Has(player, AA_G_BERSERKERS_EDGE) && player->GetHealthPct() < 30.0f)
                damage += damage / 5;

            // Furious Charge (5012) — +10/15/20% all damage window after Charge
            {
                auto it = g_furiousCharge.find(guid);
                if (it != g_furiousCharge.end() && getMSTime() < it->second.untilMs)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.15f, 0.20f, 0.20f };
                    uint8 r = std::min<uint8>(it->second.rank, 4);
                    damage += (int32)(damage * bonus[r]);
                }
            }

            // Devastate debuff (5017) — read stacking damage bonus on target
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_IMPROVED_DEVASTATE);
                if (rank > 0)
                {
                    uint32 vGuid = target->GetGUID().GetCounter();
                    auto it = g_devastate.find(guid);
                    if (it != g_devastate.end())
                    {
                        auto jt = it->second.find(vGuid);
                        if (jt != it->second.end() && jt->second.stacks > 0)
                        {
                            uint32 now = getMSTime();
                            if (now <= jt->second.expireMs)
                                damage += (int32)(damage * 0.02f * jt->second.stacks);
                        }
                    }
                }
            }

            // Unyielding Light (5126) — +dmg% spell damage while post-Divine-Shield window is active
            {
                auto it = g_unyieldingLight.find(guid);
                if (it != g_unyieldingLight.end() && getMSTime() < it->second.untilMs)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.18f, 0.30f };
                    damage += (int32)(damage * bonus[Idx<uint8>(it->second.rank)]);
                }
            }

            // Assassin's Mark (5315) — spell/ability damage bonus on marked target
            if (target)
            {
                float amBonus = 0.0f;
                if (SanctumAA_AssassinsMarkBonus(guid, target, amBonus) && amBonus > 0.0f)
                    damage += (int32)(damage * amBonus);
            }

            // ── Mark of Karna (5414) — bonus dmg on marked target ─────────────
            // Mark is set from OnPlayerSpellCast when a holy/shadow spell is cast.
            // Bonus is applied once here (only in ModifySpellDamageTaken, NOT in
            // ModifyMeleeDamage, to mirror Assassin's Mark and avoid double-dip).
            if (target)
            {
                uint32 tLow = target->GetGUID().GetCounter();
                auto it = g_markOfKarna.find(guid);
                if (it != g_markOfKarna.end() &&
                    it->second.targetLow == tLow &&
                    getMSTime() <= it->second.untilMs)
                {
                    static const float bonus[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                    damage += (int32)(damage * bonus[Idx<uint8>(it->second.rank)]);
                }
            }

            // ── Improved Power Infusion (5427) — +5/8/12% bonus to all magic damage ──
            // Applies to all spell schools. Simple flat multiplier; no PI detection needed
            // (the PI-CD-reduction half is stubbed — see STUBS below).
            if (isMagical)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PRI_IMP_POWER_INFUSION);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // ── Harbinger (5431) — SW:Death hits all enemies in 6/8/10 yd + bonus dmg ──
            // NOTE: SW:Death's self-backlash is an existing mechanic we do NOT touch.
            // We only amplify the outgoing damage and AoE the full damage+bonus to nearby enemies.
            // Safe: we use SanctumAA_DealVisibleDamage (not a re-entrant call through ModifySpellDamageTaken).
            if (target)
            {
                static const std::unordered_set<uint32> s_swdeath = { 32379, 32996 };
                if (s_swdeath.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_PRI_HARBINGER);
                    if (rank > 0)
                    {
                        static const float radii[]  = { 0.0f, 6.0f, 8.0f, 10.0f };
                        static const float bonus[]  = { 0.0f, 0.10f, 0.20f, 0.30f };
                        float r = radii[Idx<uint8>(rank)];
                        // Apply the bonus to the primary hit
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                        // AoE the (bonus-amplified) splash to nearby enemies excluding primary target
                        uint32 splashDmg = (uint32)std::max(0, damage);
                        if (splashDmg > 0)
                        {
                            std::list<Unit*> nearbyList;
                            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, r);
                            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearbyList, check);
                            Cell::VisitObjects(player, searcher, r);
                            for (Unit* u : nearbyList)
                            {
                                if (!u || u == target || !u->IsAlive()) continue;
                                SanctumAA_DealVisibleDamage(player, u, splashDmg, SPELL_SCHOOL_MASK_SHADOW);
                            }
                        }
                    }
                }
            }

            // ── Chain Reaction (5430) — Mind Blast 10/20/30% chance: 75% to random nearby enemy ──
            if (target)
            {
                static const std::unordered_set<uint32> s_mindblast = {
                    8092, 10945, 10946, 10947, 25375, 25376, 48126, 48127
                };
                if (s_mindblast.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_PRI_CHAIN_REACTION);
                    if (rank > 0 && CheckICD(guid, AA_PRI_CHAIN_REACTION, 500u))
                    {
                        static const float chance[] = { 0.0f, 10.0f, 20.0f, 30.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            uint32 splashDmg = (uint32)(std::max(0, damage) * 0.75f);
                            if (splashDmg > 0)
                            {
                                // Find a random nearby enemy within 10 yd (not primary target)
                                std::vector<Unit*> nearbyVec;
                                std::list<Unit*> nearbyList;
                                Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, 10.0f);
                                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearbyList, check);
                                Cell::VisitObjects(player, searcher, 10.0f);
                                for (Unit* u : nearbyList)
                                    if (u && u != target && u->IsAlive())
                                        nearbyVec.push_back(u);
                                if (!nearbyVec.empty())
                                {
                                    uint32 idx = urand(0, (uint32)(nearbyVec.size() - 1));
                                    SanctumAA_DealVisibleDamage(player, nearbyVec[idx], splashDmg, SPELL_SCHOOL_MASK_SHADOW);
                                }
                            }
                        }
                    }
                }
            }

            // ── Turn Undead (5416) — Holy spells vs undead <35% HP: lethal chance ──
            // Mirror of Headshot (aa_class.cpp). Placed here (attacker side, ModifySpellDamageTaken)
            // rather than aa_class.cpp to avoid double-file conflict. NOT in ModifyMeleeDamage.
            if (target && target->ToCreature() &&
                !(target->ToCreature()->isElite() || target->ToCreature()->IsDungeonBoss()))
            {
                if (schoolMask & SPELL_SCHOOL_MASK_HOLY)
                {
                    Creature const* cr = target->ToCreature();
                    if (cr && cr->GetCreatureType() == CREATURE_TYPE_UNDEAD &&
                        target->GetHealthPct() < 35.0f)
                    {
                        uint8 rank = SanctumAA::GetRank(player, AA_PRI_TURN_UNDEAD);
                        if (rank > 0 && CheckICD(guid, AA_PRI_TURN_UNDEAD, 500u))
                        {
                            static const float chance[] = { 0.0f, 5.0f, 10.0f, 20.0f };
                            if (roll_chance_f(chance[Idx<uint8>(rank)]))
                                damage = (int32)target->GetHealth();
                        }
                    }
                }
            }

            // Improved Drain Life (5803) — Drain Life ticks: increment shadow DoT vuln stack
            // on target, then read back bonus for any shadow-school DoT damage we deal.
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WRL_IMPROVED_DRAIN_LIFE);
                if (rank > 0)
                {
                    uint32 vGuid = target->GetGUID().GetCounter();
                    uint32 now   = getMSTime();

                    // Detect Drain Life periodic ticks (all ranks)
                    static const std::unordered_set<uint32> s_drainLife = {
                        689, 699, 709, 7651, 11699, 11700, 27219, 27220, 47857
                    };
                    if (s_drainLife.count(spellInfo->Id))
                    {
                        // Increment stack (cap 5), refresh 8s window
                        auto& entry = g_drainLife[guid][vGuid];
                        if (entry.stacks > 0 && now > entry.expireMs)
                            entry = DrainLifeEntry{};
                        if (entry.stacks < 5)
                            entry.stacks++;
                        entry.expireMs = now + 8000u;
                    }

                    // Read back shadow DoT vuln bonus for any shadow periodic spell
                    bool isShadow   = (spellInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_SHADOW) != 0;
                    bool isPeriodic = false;
                    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                        if (spellInfo->Effects[i].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE) { isPeriodic = true; break; }

                    if (isShadow && isPeriodic)
                    {
                        auto it = g_drainLife.find(guid);
                        if (it != g_drainLife.end())
                        {
                            auto jt = it->second.find(vGuid);
                            if (jt != it->second.end() && jt->second.stacks > 0 && now <= jt->second.expireMs)
                            {
                                static const float perStack[] = { 0.0f, 0.01f, 0.02f, 0.03f };
                                damage += (int32)(damage * perStack[Idx<uint8>(rank)] * jt->second.stacks);
                            }
                        }
                    }
                }
            }

        } // end ATTACKER IS PLAYER

        // ── VICTIM IS PLAYER ────────────────────────────────────────────────
        if (Player* player = AsPlayer(target))
        {
            uint32 vGuid = player->GetGUID().GetCounter();

            // Warding — -1/2/3% magical DR
            if (isMagical)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_WARDING);
                if (rank > 0)
                {
                    static const float dr[] = { 0.0f, 0.01f, 0.02f, 0.03f };
                    damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                }
            }

            // Hardening
            {
                auto it = g_hardening.find(vGuid);
                if (it != g_hardening.end() && it->second.stacks > 0)
                    damage = (int32)(damage * (1.0f - 0.01f * it->second.stacks));
            }

            // Bulwark
            {
                uint8 rank = SanctumAA::GetRank(player, AA_G_BULWARK);
                if (rank > 0)
                {
                    static const float capPct[] = { 0.0f, 0.35f, 0.30f, 0.25f };
                    int32 cap = (int32)(player->GetMaxHealth() * capPct[Idx<uint8>(rank)]);
                    if (damage > cap)
                        damage = cap;
                }
            }

            // Hindsight
            {
                auto it = g_hindsight.find(vGuid);
                if (it != g_hindsight.end() && it->second.absorb > 0)
                {
                    if (getMSTime() > it->second.expireMs)
                        it->second.absorb = 0;
                    else
                    {
                        int32 absorbed = std::min(it->second.absorb, damage);
                        damage -= absorbed;
                        it->second.absorb -= absorbed;
                    }
                }
            }

            // Iron Warrior (5013) — DR scaling with missing HP + R3 absorb shield
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_IRON_WARRIOR);
                if (rank > 0 && damage > 0)
                {
                    static const float maxDR[] = { 0.0f, 0.10f, 0.15f, 0.20f };
                    float hpPct = player->GetHealthPct() / 100.0f;
                    float dr    = maxDR[Idx<uint8>(rank)] * (1.0f - hpPct);
                    if (dr > 0.0f)
                        damage = (int32)(damage * (1.0f - dr));

                    auto it = g_ironWarriorAbsorb.find(vGuid);
                    if (it != g_ironWarriorAbsorb.end() && it->second.absorb > 0)
                    {
                        if (getMSTime() > it->second.expireMs)
                            it->second.absorb = 0;
                        else
                        {
                            int32 absorbed = std::min(it->second.absorb, damage);
                            damage -= absorbed;
                            it->second.absorb -= absorbed;
                        }
                    }
                }
            }

            // Battle Endurance (5014) — intercept killing blow once per combat
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_BATTLE_ENDURANCE);
                if (rank > 0 && player->IsAlive() && damage >= (int32)player->GetHealth())
                {
                    auto& be = g_battleEndurance[vGuid];
                    if (!be.used)
                    {
                        be.used = true;
                        static const float survivePct[] = { 0.0f, 0.10f, 0.15f };
                        float sp = survivePct[std::min<uint8>(rank, 2)];
                        int32 surviveHP = (int32)(player->GetMaxHealth() * sp);
                        damage = (int32)player->GetHealth() > surviveHP ? ((int32)player->GetHealth() - surviveHP) : 0;

                        auto& hot = g_beHot[vGuid];
                        hot.pool       = (int32)(player->GetMaxHealth() * 0.20f);
                        hot.lastTickMs = getMSTime();

                        if (rank >= 2)
                            be.shieldUntil = getMSTime() + 4000u;
                    }
                }
            }

            // Battle Endurance R2 — -30% damage taken DR window
            {
                auto it = g_battleEndurance.find(vGuid);
                if (it != g_battleEndurance.end() && it->second.shieldUntil > 0)
                {
                    if (getMSTime() < it->second.shieldUntil)
                        damage = (int32)(damage * 0.70f);
                    else
                        it->second.shieldUntil = 0;
                }
            }

            // Retaliation (5011) R2: also reflects spell/ability damage
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_RETALIATION);
                if (rank >= 2 && damage > 0 && attacker)
                {
                    uint32 reflectAmt = (uint32)(damage * 0.25f);
                    if (reflectAmt > 0)
                    {
                        auto& ret = g_retaliation[vGuid];
                        ret.amount      += reflectAmt;
                        ret.attackerGuid = attacker->GetGUID().GetRawValue();
                    }
                }
            }

            // Improved Avenger's Shield (5110) — read attacker debuff: reduce spell dmg dealt
            if (attacker)
            {
                uint32 atkGuid = attacker->GetGUID().GetCounter();
                auto pit = g_avengerDebuff.find(vGuid);
                if (pit != g_avengerDebuff.end())
                {
                    auto ait = pit->second.find(atkGuid);
                    if (ait != pit->second.end() && getMSTime() < ait->second.expireMs)
                    {
                        damage = (int32)(damage * (1.0f - ait->second.dr));
                    }
                }
            }

            // Sanctuary (5113) — pool heal when Blessing of Sanctuary aura is active (spell dmg hit)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_SANCTUARY);
                if (rank > 0 && damage > 0)
                {
                    bool hasSanct = false;
                    for (uint32 id : s_blessSanctuary)
                        if (player->HasAura(id)) { hasSanct = true; break; }
                    if (hasSanct)
                    {
                        static const float pct[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                        int32 healAmt = (int32)(player->GetMaxHealth() * pct[Idx<uint8>(rank)]);
                        if (healAmt > 0)
                            g_sanctuaryHealPool[vGuid] += healAmt;
                    }
                }
            }

            // Celestial Barrier (5420) — also absorbs spell damage (same absorb pool as melee)
            {
                auto it = g_celestialBarrier.find(vGuid);
                if (it != g_celestialBarrier.end() && it->second.absorb > 0 &&
                    getMSTime() < it->second.expireMs)
                {
                    int32 absorbed = std::min((int32)damage, it->second.absorb);
                    it->second.absorb -= absorbed;
                    damage -= absorbed;
                    if (damage < 0) damage = 0;
                    if (it->second.absorb <= 0)
                        g_celestialBarrier.erase(it);
                }
            }

        } // end VICTIM IS PLAYER
    }

    // -----------------------------------------------------------------------
    // ModifyPeriodicDamageAurasTick — DoT tick modifier (School Mastery).
    // -----------------------------------------------------------------------
    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (damage == 0 || !spellInfo)
            return;

        Player* player = AsPlayer(attacker);
        if (!player)
            return;

        uint32 schoolMask = spellInfo->GetSchoolMask();
        static const float bonus[] = { 0.0f, 0.03f, 0.07f, 0.12f };

        uint8 rank = 0;
        if      (schoolMask & SPELL_SCHOOL_MASK_FIRE)   rank = SanctumAA::GetRank(player, AA_G_SCHOOL_FIRE);
        else if (schoolMask & SPELL_SCHOOL_MASK_FROST)  rank = SanctumAA::GetRank(player, AA_G_SCHOOL_FROST);
        else if (schoolMask & SPELL_SCHOOL_MASK_SHADOW) rank = SanctumAA::GetRank(player, AA_G_SCHOOL_SHADOW);
        else if (schoolMask & SPELL_SCHOOL_MASK_HOLY)   rank = SanctumAA::GetRank(player, AA_G_SCHOOL_HOLY);
        else if (schoolMask & SPELL_SCHOOL_MASK_NATURE) rank = SanctumAA::GetRank(player, AA_G_SCHOOL_NATURE);
        else if (schoolMask & SPELL_SCHOOL_MASK_ARCANE) rank = SanctumAA::GetRank(player, AA_G_SCHOOL_ARCANE);
        else if (schoolMask & SPELL_SCHOOL_MASK_NORMAL) rank = SanctumAA::GetRank(player, AA_G_SCHOOL_PHYSICAL);

        if (rank > 0)
            damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);

        // ── Encroaching Darkness (5433) — SW:P/VT/DP +5/10/15% per DoT tick ──
        // Placed in ModifyPeriodicDamageAurasTick (not ModifySpellDamageTaken) because
        // periodic ticks are routed through this hook, not the spell-hit hook.
        {
            static const std::unordered_set<uint32> s_shadowDots = {
                // Shadow Word: Pain all ranks
                589, 594, 970, 992, 2767, 10892, 10893, 25367, 48124, 48125,
                // Vampiric Touch all ranks
                34914, 34916, 34917, 48159, 48160,
                // Devouring Plague all ranks
                2944, 19276, 19277, 19278, 25467, 48300, 48301
            };
            if (s_shadowDots.count(spellInfo->Id))
            {
                uint8 eRank = SanctumAA::GetRank(player, AA_PRI_ENCROACHING_DARKNESS);
                if (eRank > 0)
                {
                    static const float eBonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    damage += (uint32)(damage * eBonus[Idx<uint8>(eRank)]);
                }
            }
        }

        // ── Spreading Misery (5428) R2 — +10% shadow damage on all shadow DoT ticks ──
        // (Kill-trigger for jumping diseases is in aa_class.cpp OnPlayerCreatureKill)
        {
            uint8 smRank = SanctumAA::GetRank(player, AA_PRI_SPREADING_MISERY);
            if (smRank >= 2 && (schoolMask & SPELL_SCHOOL_MASK_SHADOW))
            {
                damage += (uint32)(damage * 0.10f);
            }
        }
    }

    // -----------------------------------------------------------------------
    // ModifyHealReceived — PART B: heal modifier hook.
    //
    // Used by:
    //   4301  Mending Touch       — healer AA: +5/10/15% healing done from all sources
    //   5114  Improved Flash of Light — Paladin: at 5 Radiance stacks, next Flash heals +30/45/60% more
    //   5115  Improved Seal of Light  — Paladin: Seal/Judgement of Light heals +20/35/50%
    //   5117  Lay of Hands Mastery    — Paladin: Lay on Hands +25/50/100% healing
    //   5126  Unyielding Light        — Paladin: +10/18/30% healing received while window active
    // -----------------------------------------------------------------------
    void ModifyHealReceived(Unit* target, Unit* healer, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (heal == 0 || !healer || !spellInfo)
            return;

        // ── HEALER IS PLAYER ──────────────────────────────────────────────────
        if (Player* hPlayer = AsPlayer(healer))
        {
            // Mending Touch (4301) — +5/10/15% all healing done
            {
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_H_MENDING_TOUCH);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    heal += (uint32)(heal * bonus[Idx<uint8>(rank)]);
                }
            }

            uint32 hGuid = hPlayer->GetGUID().GetCounter();

            // Improved Flash of Light (5114) — at 5 Radiance stacks, next Flash heals more
            {
                static const std::unordered_set<uint32> s_fol = {
                    19750, 19939, 19940, 19941, 19942, 25363, 27137, 48784, 48785
                };
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_PAL_IMPROVED_FLASH_OF_LIGHT);
                if (rank > 0 && s_fol.count(spellInfo->Id))
                {
                    auto it = g_radianceStacks.find(hGuid);
                    if (it != g_radianceStacks.end() && it->second >= 5)
                    {
                        static const float bonus[] = { 0.0f, 0.30f, 0.45f, 0.60f };
                        heal += (uint32)(heal * bonus[Idx<uint8>(rank)]);
                        it->second = 0; // consume all stacks
                    }
                }
            }

            // Improved Seal of Light (5115) — Seal of Light / Judgement of Light heals +20/35/50%
            // These are the actual heal spells triggered by Seal/JoL, NOT the initial judgement spells.
            {
                static const std::unordered_set<uint32> s_solHeal = {
                    // Seal of Light heal procs (all ranks)
                    20167, 20333, 20334, 20335,
                    // Judgement of Light heal
                    20185
                };
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_PAL_IMPROVED_SEAL_OF_LIGHT);
                if (rank > 0 && s_solHeal.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                    heal += (uint32)(heal * bonus[Idx<uint8>(rank)]);
                }
            }

            // Lay of Hands Mastery (5117) — +25/50/100% LoH healing
            {
                static const std::unordered_set<uint32> s_loh = { 633, 2800, 10310, 27154, 48788 };
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_PAL_LAY_OF_HANDS_MASTERY);
                if (rank > 0 && s_loh.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.25f, 0.50f, 1.00f };
                    heal += (uint32)(heal * bonus[Idx<uint8>(rank)]);
                }
            }
            // ── Twinheal (5401) — 5/10/15% chance to double heal ──────────────
            // Guarded by a per-GUID 50ms ICD to prevent double-doubling (Twincast interaction).
            {
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_PRI_TWINHEAL);
                if (rank > 0)
                {
                    static const float chance[] = { 0.0f, 5.0f, 10.0f, 15.0f };
                    if (CheckICD(hGuid, AA_PRI_TWINHEAL, 50u) &&
                        roll_chance_f(chance[Idx<uint8>(rank)]))
                    {
                        heal *= 2;
                    }
                }
            }

            // ── Channeling the Divine (5403) — consume a double-heal charge ──
            {
                auto it = g_channelingDivine.find(hGuid);
                if (it != g_channelingDivine.end() && it->second.charges > 0)
                {
                    heal *= 2;
                    --it->second.charges;
                }
            }

            // ── Gift of Mana (5402) — 5/10/15% chance to set "next spell free" flag ──
            {
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_PRI_GIFT_OF_MANA);
                if (rank > 0)
                {
                    // Only set if the flag isn't already pending (avoid waste)
                    auto flagIt = g_giftOfMana.find(hGuid);
                    bool alreadyPending = (flagIt != g_giftOfMana.end() && flagIt->second);
                    if (!alreadyPending)
                    {
                        static const float chance[] = { 0.0f, 5.0f, 10.0f, 15.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                            g_giftOfMana[hGuid] = true;
                    }
                }
            }

        } // end HEALER IS PLAYER

        // ── TARGET IS PLAYER — incoming heal buffs ─────────────────────────
        if (Player* tPlayer = AsPlayer(target))
        {
            uint32 tGuid = tPlayer->GetGUID().GetCounter();

            // Unyielding Light (5126) — +10/18/30% healing received while post-Divine-Shield window is active
            {
                auto it = g_unyieldingLight.find(tGuid);
                if (it != g_unyieldingLight.end() && getMSTime() < it->second.untilMs)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.18f, 0.30f };
                    heal += (uint32)(heal * bonus[Idx<uint8>(it->second.rank)]);
                }
            }
        } // end TARGET IS PLAYER
    }

    // -----------------------------------------------------------------------
    // OnUnitUpdate — periodic passive effects (heals, DR resets).
    // -----------------------------------------------------------------------
    void OnUnitUpdate(Unit* unit, uint32 /*diff*/) override
    {
        Player* player = AsPlayer(unit);
        if (!player)
            return;
        if (!player->IsAlive())
            return;

        uint32 guid = player->GetGUID().GetCounter();
        uint32 now  = getMSTime();

        // Reanimation — below 20% HP: regen 2/4/6% of missing HP per second
        {
            uint8 rank = SanctumAA::GetRank(player, AA_G_REANIMATION);
            if (rank > 0 && player->GetHealthPct() < 20.0f && !player->IsFullHealth())
            {
                auto& lastTick = g_reanimTick[guid];
                if (GetMSTimeDiffToNow(lastTick) >= 1000u)
                {
                    static const float pct[] = { 0.0f, 0.02f, 0.04f, 0.06f };
                    int32 missing  = (int32)player->GetMaxHealth() - (int32)player->GetHealth();
                    int32 healAmt  = std::max(1, (int32)(missing * pct[Idx<uint8>(rank)]));
                    player->ModifyHealth(healAmt);
                    lastTick = now;
                }
            }
        }

        // Natural Renewal
        {
            uint8 rank = SanctumAA::GetRank(player, AA_G_NATURAL_RENEWAL);
            if (rank > 0 && !player->IsFullHealth())
            {
                static const int32 healAmt[] = { 0, 25, 60, 110 };
                auto& r = g_renewal[guid];
                if (GetMSTimeDiffToNow(r.lastTickMs) >= 5000u)
                {
                    player->ModifyHealth(healAmt[Idx<uint8>(rank)]);
                    r.lastTickMs = now;
                }
            }
        }

        // Recovery
        {
            auto it = g_recovery.find(guid);
            if (it != g_recovery.end() && it->second.pool > 0)
            {
                auto& r = it->second;
                if (GetMSTimeDiffToNow(r.lastTickMs) >= 1000u)
                {
                    int32 tickHeal = std::max(1, r.pool / 6);
                    player->ModifyHealth(tickHeal);
                    r.pool -= tickHeal;
                    if (r.pool < 0)
                        r.pool = 0;
                    r.lastTickMs = now;
                }
            }
        }

        // Hardening — reset stacks when player leaves combat
        {
            auto it = g_hardening.find(guid);
            if (it != g_hardening.end() && it->second.stacks > 0 && !player->IsInCombat())
                it->second.stacks = 0;
        }

        // Battle Endurance HoT — 20% max HP healed back over 4s (1s ticks)
        {
            auto it = g_beHot.find(guid);
            if (it != g_beHot.end() && it->second.pool > 0)
            {
                auto& h = it->second;
                if (GetMSTimeDiffToNow(h.lastTickMs) >= 1000u)
                {
                    int32 tickHeal = std::max(1, h.pool / 4);
                    player->ModifyHealth(tickHeal);
                    h.pool -= tickHeal;
                    if (h.pool < 0) h.pool = 0;
                    h.lastTickMs = now;
                }
            }
        }

        // Battle Endurance — reset used flag when out of combat
        {
            auto it = g_battleEndurance.find(guid);
            if (it != g_battleEndurance.end() && it->second.used && !player->IsInCombat())
                it->second.used = false;
        }

        // Retaliation — flush pending reflect to attacker
        {
            auto it = g_retaliation.find(guid);
            if (it != g_retaliation.end() && it->second.amount > 0)
            {
                ObjectGuid attackerGuid = ObjectGuid(it->second.attackerGuid);
                Unit* attackerUnit = ObjectAccessor::GetUnit(*player, attackerGuid);
                if (attackerUnit && attackerUnit->IsAlive())
                {
                    Unit::DealDamage(player, attackerUnit, it->second.amount, nullptr,
                                     DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                }
                it->second.amount = 0;
                it->second.attackerGuid = 0;
            }
        }

        // Devastate — expire stale stacks
        {
            auto it = g_devastate.find(guid);
            if (it != g_devastate.end())
            {
                uint32 now2 = getMSTime();
                for (auto& [vg, entry] : it->second)
                {
                    if (entry.stacks > 0 && now2 > entry.expireMs)
                        entry = DevastateEntry{};
                }
            }
        }

        // Improved Drain Life (5803) — expire stale debuff stacks
        {
            auto it = g_drainLife.find(guid);
            if (it != g_drainLife.end())
            {
                uint32 now2 = getMSTime();
                for (auto& [vg, entry] : it->second)
                {
                    if (entry.stacks > 0 && now2 > entry.expireMs)
                        entry = DrainLifeEntry{};
                }
            }
        }

        // Sanctuary (5113) — flush pooled heals each update tick
        {
            auto it = g_sanctuaryHealPool.find(guid);
            if (it != g_sanctuaryHealPool.end() && it->second > 0 && !player->IsFullHealth())
            {
                player->ModifyHealth(it->second);
                it->second = 0;
            }
        }

        // Unyielding Light (5126) — edge-detect Divine Shield expiry to open the window
        {
            uint8 rank = SanctumAA::GetRank(player, AA_PAL_UNYIELDING_LIGHT);
            if (rank > 0)
            {
                static const std::unordered_set<uint32> s_divineShield = { 642, 1020, 1022 };
                bool dsActive = false;
                for (uint32 id : s_divineShield)
                    if (player->HasAura(id)) { dsActive = true; break; }

                bool& wasActive = g_divineShieldWasActive[guid];
                if (wasActive && !dsActive)
                {
                    // Divine Shield just expired — open Unyielding Light window
                    g_unyieldingLight[guid] = { getMSTime() + 8000u, rank };
                }
                wasActive = dsActive;
            }
        }

        // Unyielding Light — expire stale windows
        {
            auto it = g_unyieldingLight.find(guid);
            if (it != g_unyieldingLight.end() && getMSTime() >= it->second.untilMs)
                g_unyieldingLight.erase(it);
        }

        // Avenger's Shield debuff — expire stale entries
        {
            auto pit = g_avengerDebuff.find(guid);
            if (pit != g_avengerDebuff.end())
            {
                uint32 now2 = getMSTime();
                std::vector<uint32> toErase;
                for (auto& [vg, dbf] : pit->second)
                    if (now2 >= dbf.expireMs) toErase.push_back(vg);
                for (uint32 vg : toErase) pit->second.erase(vg);
            }
        }
    }

    // -----------------------------------------------------------------------
    // OnUnitDeath
    // -----------------------------------------------------------------------
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        uint32 deadGuid = unit->GetGUID().GetCounter();

        for (auto& [ag, victimMap] : g_thousandCuts)
            victimMap.erase(deadGuid);
        for (auto& [ag, victimMap] : g_drainLife)
            victimMap.erase(deadGuid);
        for (auto& [ag, victimMap] : g_puncture)
            victimMap.erase(deadGuid);

        if (unit->IsPlayer())
            ClearPlayerState(deadGuid);
    }

    // -----------------------------------------------------------------------
    // OnUnitEnterEvadeMode — creature reset clears Thousand Cuts + Devastate stacks.
    // -----------------------------------------------------------------------
    void OnUnitEnterEvadeMode(Unit* unit, uint8 /*reason*/) override
    {
        uint32 evadeGuid = unit->GetGUID().GetCounter();
        for (auto& [ag, victimMap] : g_thousandCuts)
            victimMap.erase(evadeGuid);
        for (auto& [ag, victimMap] : g_devastate)
            victimMap.erase(evadeGuid);
        for (auto& [ag, victimMap] : g_drainLife)
            victimMap.erase(evadeGuid);
        for (auto& [ag, victimMap] : g_puncture)
            victimMap.erase(evadeGuid);
    }
};

// ---------------------------------------------------------------------------
// aa_combat_player — PlayerScript for Twincast and Apex Predator on-kill
// ---------------------------------------------------------------------------
class aa_combat_player : public PlayerScript
{
public:
    aa_combat_player() : PlayerScript("aa_combat_player") {}

    // Twincast — 5/10/15% chance any direct-damage spell fires a second time.
    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override
    {
        if (g_inTwincast || skipCheck)
            return;

        uint8 rank = SanctumAA::GetRank(player, AA_D_TWINCAST);
        if (!rank)
            return;

        SpellInfo const* info = spell->GetSpellInfo();
        if (!info)
            return;

        bool isDamage = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 effect = info->Effects[i].Effect;
            if (effect == SPELL_EFFECT_SCHOOL_DAMAGE      ||
                effect == SPELL_EFFECT_WEAPON_DAMAGE      ||
                effect == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL)
            {
                isDamage = true;
                break;
            }
        }
        if (!isDamage)
            return;

        static const float chance[] = { 0.0f, 5.0f, 10.0f, 15.0f };
        if (!roll_chance_f(chance[Idx<uint8>(rank)]))
            return;

        if (!CheckICD(player->GetGUID().GetCounter(), AA_D_TWINCAST + info->Id, 1000u))
            return;

        Unit* spellTarget = spell->m_targets.GetUnitTarget();
        if (!spellTarget)
            return;

        g_inTwincast = true;
        player->CastSpell(spellTarget, info->Id, true);
        g_inTwincast = false;
    }

    // Clean up all state on logout
    void OnPlayerLogout(Player* player) override
    {
        ClearPlayerState(player->GetGUID().GetCounter());
    }
};

// ---------------------------------------------------------------------------
// Registration — called from mod-aa-system_loader.cpp
// ---------------------------------------------------------------------------
void AddSC_aa_combat_modifiers()
{
    new aa_combat_unit();
    new aa_combat_player();
}
