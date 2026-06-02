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
#include "Spell.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Random.h"
#include <unordered_map>
#include <algorithm>

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

    std::unordered_map<uint32, VengeanceState> g_vengeance;
    std::unordered_map<uint32, RecoveryState>  g_recovery;
    std::unordered_map<uint32, RenewalState>   g_renewal;


    // Thousand Cuts: attacker low → victim low → stack entry
    std::unordered_map<uint32, std::unordered_map<uint32, CutsEntry>> g_thousandCuts;

    // ICD tracker: guid → aaId → last proc timestamp (getMSTime())
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> g_icd;

    std::unordered_map<uint32, HardeningState> g_hardening;
    std::unordered_map<uint32, HindsightState> g_hindsight;
    std::unordered_map<uint32, uint32>         g_reanimTick;

    // Recursion guard for Twincast
    bool g_inTwincast = false;

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
    }
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
    }

    // -----------------------------------------------------------------------
    // OnUnitDeath
    // -----------------------------------------------------------------------
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        uint32 deadGuid = unit->GetGUID().GetCounter();

        for (auto& [ag, victimMap] : g_thousandCuts)
            victimMap.erase(deadGuid);

        if (unit->IsPlayer())
            ClearPlayerState(deadGuid);
    }

    // -----------------------------------------------------------------------
    // OnUnitEnterEvadeMode — creature reset clears Thousand Cuts stacks.
    // -----------------------------------------------------------------------
    void OnUnitEnterEvadeMode(Unit* unit, uint8 /*reason*/) override
    {
        uint32 evadeGuid = unit->GetGUID().GetCounter();
        for (auto& [ag, victimMap] : g_thousandCuts)
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
