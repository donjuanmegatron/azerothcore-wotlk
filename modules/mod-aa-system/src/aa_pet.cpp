// aa_pet.cpp
//
// Sanctum AA System — Pet tree hook-based AAs.
//
// IMPLEMENTED HERE:
//   3001  Command           — +5/10/15% pet damage (ModifyMeleeDamage attacker=pet/guardian)
//   3002  Master's Bond     — +12/25/40% pet damage (stacks with Command)
//   3003  Pack Tactics      — +3/6/10% pet crit (Agility boost at spawn)
//   3005  Savage Flurry     — 5/10/15% chance pet auto hits third time for 50% dmg. 200ms ICD.
//   3101  Hardened Hide     — +flat armor at spawn (~10/20/30%)
//   3102  Iron Constitution — +flat HP at spawn (~5/10/15%)
//   3103  Handler           — -5/10/15% pet/guardian dmg taken
//   3104  Uncrushable       — large armor bonus (crit/crush immunity proxy)
//   3105  Steeled Resolve   — pet dodge/parry/block/defense ratings at spawn
//   3106  Guardian's Resolve— while pet/guardian holds threat: -3/6/10% dmg from that attacker
//   3203  Pack Leader       — pet/guardian attacks heal owner for 2/4/6% of damage dealt
//   5234  Pet Attunement    — Hunter: pet/guardian inherits 10/20/30% of owner's armor (at spawn)
//
// All AAs apply to BOTH the native WoW pet slot (Pet*) AND guardian TempSummon
// creatures (Felguard, Risen Ghoul, Spirit Wolf, Treant, Shadowfiend).
// GetOwnerPlayer() detects both types by checking GetOwner() for a Player.
//
// DEFERRED:
//   3004  Predator's Howl   — needs spell ID for debuff apply
//   3201  Assist Me         — active ability → aa_actives.cpp
//   3202  Redirection       — complex spell intercept mechanic → deferred
//   3204  Soul Bond         — complex pet re-summon on death → deferred
//   3205  Stat Inheritance  — cross-module change in mod-pet-systems → deferred

#include "aa_runtime.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Unit.h"
#include "SpellInfo.h"
#include "Timer.h"
#include "Random.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

// Cheer active-burst windows — defined in aa_actives.cpp.
extern bool SanctumAA_CheerOffensiveActive(uint32 guid);
extern bool SanctumAA_CheerDefensiveActive(uint32 guid);
extern bool SanctumAA_CheerSwiftnessActive(uint32 guid);

// ---------------------------------------------------------------------------
// File-local state
// ---------------------------------------------------------------------------
namespace
{
    // Unit GUIDs that have had spawn-time AA stats applied this session.
    // Cleared on death so re-summons re-apply cleanly.
    std::unordered_set<uint32> g_petStatsApplied;

    // Per-unit Savage Flurry ICD: unitGuid → last proc timestamp
    std::unordered_map<uint32, uint32> g_sfIcd;

    // Mending Bond regen tick: unitGuid → last tick timestamp (1s throttle)
    std::unordered_map<uint32, uint32> g_mendBondTick;

    // Clamp rank for array index.
    template<typename T>
    static inline T Idx(uint8 rank) { return static_cast<T>(std::min<uint8>(rank, 3)); }

    // Returns the Player owner of a unit if it is either:
    //   a) A real WoW Pet* (native pet slot), or
    //   b) A guardian Creature* (TempSummon) with a player owner GUID.
    // Returns nullptr for anything else (world creatures, players, etc.)
    static inline Player* GetOwnerPlayer(Unit* u)
    {
        if (!u) return nullptr;

        // Real pet slot — IsPet() is true only for Pet* objects.
        if (u->IsPet())
        {
            Pet* pet = u->ToPet();
            Unit* owner = pet ? pet->GetOwner() : nullptr;
            return owner ? owner->ToPlayer() : nullptr;
        }

        // Guardian TempSummon — Creature* with an owner GUID set via SetOwnerGUID().
        // Only creatures that were explicitly summoned by a player have this.
        Unit* owner = u->GetOwner();
        if (owner && owner->GetTypeId() == TYPEID_PLAYER)
            return owner->ToPlayer();

        return nullptr;
    }

    // Apply all spawn-time stat passives to a pet or guardian unit (once per summon).
    // Works on Unit* so it handles both Pet* and guardian Creature*.
    static void ApplyPetStatAAs(Unit* unit, Player* player)
    {
        // ── Pack Tactics — +3/6/10% pet crit chance via Agility boost ───────────
        // ApplyRatingMod is Player-only; Agility gives pets crit + dodge indirectly.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_PACK_TACTICS);
            if (rank > 0)
            {
                unit->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY, TOTAL_VALUE, 173.0f * rank, true);
                unit->UpdateAllStats();
            }
        }

        // ── Hardened Hide — flat armor bonus (~10/20/30% of a mid-range pet) ──────
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_HARDENED_HIDE);
            if (rank > 0)
            {
                unit->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, 800.0f * rank, true);
                unit->UpdateArmor();
            }
        }

        // ── Iron Constitution — flat HP bonus (~5/10/15%) ────────────────────────
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_IRON_CONSTITUTION);
            if (rank > 0)
            {
                unit->HandleStatFlatModifier(UNIT_MOD_HEALTH, TOTAL_VALUE, 400.0f * rank, true);
                unit->UpdateMaxHealth();
            }
        }

        // ── Uncrushable — large armor bonus (proxy for crit/crush immunity) ──────
        if (SanctumAA::Has(player, AA_P_UNCRUSHABLE))
        {
            unit->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, 4000.0f, true);
            unit->UpdateArmor();
        }

        // ── Steeled Resolve — Agility (avoidance proxy) + Armor (defense proxy) ──
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_STEELED_RESOLVE);
            if (rank > 0)
            {
                unit->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY, TOTAL_VALUE, 113.0f * rank, true);
                unit->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, 370.0f * rank, true);
                unit->UpdateAllStats();
                unit->UpdateArmor();
            }
        }

        // ── Pet Attunement (Hunter) — pet/guardian inherits 10/20/30% of owner's armor ──
        {
            uint8 rank = SanctumAA::GetRank(player, AA_HUN_PET_ATTUNEMENT);
            if (rank > 0)
            {
                static const float pct[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                float bonusArmor = player->GetArmor() * pct[Idx<uint8>(rank)];
                unit->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, bonusArmor, true);
                unit->UpdateArmor();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// aa_pet_unit — UnitScript for per-hit pet AAs
// ---------------------------------------------------------------------------
class aa_pet_unit : public UnitScript
{
public:
    aa_pet_unit() : UnitScript("aa_pet_unit", true,
    {
        UNITHOOK_ON_DAMAGE,
        UNITHOOK_MODIFY_MELEE_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_ON_UNIT_UPDATE,
        UNITHOOK_ON_UNIT_DEATH,
    }) {}

    // -----------------------------------------------------------------------
    // OnDamage — fires after damage is applied.
    //   Pack Leader — pet/guardian deals damage → heal the owner.
    // -----------------------------------------------------------------------
    void OnDamage(Unit* attacker, Unit* /*victim*/, uint32& damage) override
    {
        if (damage == 0) return;

        Player* player = GetOwnerPlayer(attacker);
        if (!player) return;

        // Pack Leader — heal owner for 2/4/6% of pet/guardian damage dealt
        uint8 rank = SanctumAA::GetRank(player, AA_P_PACK_LEADER);
        if (rank > 0)
        {
            static const float pct[] = { 0.0f, 0.02f, 0.04f, 0.06f };
            int32 healAmt = std::max(1, (int32)(damage * pct[Idx<uint8>(rank)]));
            if (!player->IsFullHealth())
                player->ModifyHealth(healAmt);
        }
    }

    // -----------------------------------------------------------------------
    // ModifyMeleeDamage — fires for all melee auto-attacks.
    //
    // Attacker = pet/guardian: Command, Master's Bond, Savage Flurry.
    // Victim   = pet/guardian: Handler, Guardian's Resolve.
    // -----------------------------------------------------------------------
    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (damage == 0)
            return;

        // ── ATTACKER IS PET OR GUARDIAN ─────────────────────────────────────
        {
            Player* player = GetOwnerPlayer(attacker);
            if (player)
            {
                // Command — +5/10/15% pet/guardian damage
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_COMMAND);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }

                // Master's Bond — +12/25/40% pet/guardian damage (stacks with Command)
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_MASTERS_BOND);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.12f, 0.25f, 0.40f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }

                // Bloodscent — +10/20/30% pet/guardian damage to targets below 35% HP
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_BLOODSCENT);
                    if (rank > 0 && target && target->GetHealthPct() < 35.0f)
                    {
                        static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }

                // Cheer: Offensive — passive +3/5/8% pet damage, +8/15/25% during the active burst
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_HUN_CHEER_OFFENSIVE);
                    if (rank > 0)
                    {
                        static const float passive[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                        float bonus = passive[Idx<uint8>(rank)];
                        if (SanctumAA_CheerOffensiveActive(player->GetGUID().GetCounter()))
                        {
                            static const float burst[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                            bonus += burst[Idx<uint8>(rank)];
                        }
                        damage += (uint32)(damage * bonus);
                    }
                }

                // Savage Flurry — 5/10/15% chance: add 50% extra dmg (third hit). 200ms ICD.
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_SAVAGE_FLURRY);
                    if (rank > 0)
                    {
                        uint32 unitGuid = attacker->GetGUID().GetCounter();
                        auto&  stamp    = g_sfIcd[unitGuid];
                        if (GetMSTimeDiffToNow(stamp) >= 200u)
                        {
                            static const float chance[] = { 0.0f, 5.0f, 10.0f, 15.0f };
                            if (roll_chance_f(chance[Idx<uint8>(rank)]))
                            {
                                damage += damage / 2u;
                                stamp = getMSTime();
                            }
                        }
                    }
                }
            }
        }

        // ── VICTIM IS PET OR GUARDIAN ────────────────────────────────────────
        {
            Player* player = GetOwnerPlayer(target);
            if (player)
            {
                // Handler — -5/10/15% flat DR on pet/guardian
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_HANDLER);
                    if (rank > 0)
                    {
                        static const float dr[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }

                // Guardian's Resolve — -3/6/10% DR while this attacker is targeting the pet/guardian
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_GUARDIANS_RESOLVE);
                    if (rank > 0 && attacker && attacker->GetVictim() == target)
                    {
                        static const float dr[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                        damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }

                // Cheer: Defensive — passive -3/5/8% dmg taken, -8/15/25% during the active burst
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_HUN_CHEER_DEFENSIVE);
                    if (rank > 0)
                    {
                        static const float passive[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                        float dr = passive[Idx<uint8>(rank)];
                        if (SanctumAA_CheerDefensiveActive(player->GetGUID().GetCounter()))
                        {
                            static const float burst[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                            dr += burst[Idx<uint8>(rank)];
                        }
                        damage = (uint32)(damage * (1.0f - dr));
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // ModifySpellDamageTaken — spell damage to the pet/guardian.
    //   Handler, Guardian's Resolve apply to all damage types.
    // -----------------------------------------------------------------------
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0) return;

        Player* player = GetOwnerPlayer(target);
        if (!player) return;

        // Handler
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_HANDLER);
            if (rank > 0)
            {
                static const float dr[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
            }
        }

        // Guardian's Resolve
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_GUARDIANS_RESOLVE);
            if (rank > 0 && attacker && attacker->GetVictim() == target)
            {
                static const float dr[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
            }
        }

        // Cheer: Defensive — passive -3/5/8% dmg taken, -8/15/25% during the active burst
        {
            uint8 rank = SanctumAA::GetRank(player, AA_HUN_CHEER_DEFENSIVE);
            if (rank > 0)
            {
                static const float passive[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                float dr = passive[Idx<uint8>(rank)];
                if (SanctumAA_CheerDefensiveActive(player->GetGUID().GetCounter()))
                {
                    static const float burst[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                    dr += burst[Idx<uint8>(rank)];
                }
                damage = (int32)(damage * (1.0f - dr));
            }
        }
    }

    // -----------------------------------------------------------------------
    // OnUnitUpdate — apply spawn-time stat passives on the first tick.
    //
    // Fires for every unit each world update cycle. The GetOwnerPlayer() check
    // gates to only Pet* or guardian Creature* units owned by a player — all
    // other world creatures bail out immediately. The g_petStatsApplied set
    // makes the per-owned-unit path O(1) after the first application.
    // -----------------------------------------------------------------------
    void OnUnitUpdate(Unit* unit, uint32 /*diff*/) override
    {
        Player* player = GetOwnerPlayer(unit);
        if (!player) return;

        uint32 unitGuid = unit->GetGUID().GetCounter();
        uint32 now      = getMSTime();

        // Mending Bond — regen 2/4/6% max HP per second while below 50% HP
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_MENDING_BOND);
            if (rank > 0 && unit->IsAlive() && unit->GetHealthPct() < 50.0f && !unit->IsFullHealth())
            {
                auto& stamp = g_mendBondTick[unitGuid];
                if (GetMSTimeDiffToNow(stamp) >= 1000u)
                {
                    stamp = now;
                    static const float pct[] = { 0.0f, 0.02f, 0.04f, 0.06f };
                    int32 healAmt = (int32)(unit->GetMaxHealth() * pct[Idx<uint8>(rank)]);
                    if (healAmt > 0)
                        unit->ModifyHealth(healAmt);
                }
            }
        }

        // Cheer: Swiftness — passive +8/15/20% pet move speed, +20/35/50% during the burst.
        // Maintained each tick; only writes the speed rate when it actually changes, so the
        // burst automatically reverts to the passive level when the window expires.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_HUN_CHEER_SWIFTNESS);
            if (rank > 0)
            {
                static const float passive[] = { 0.0f, 0.08f, 0.15f, 0.20f };
                float rate = 1.0f + passive[Idx<uint8>(rank)];
                if (SanctumAA_CheerSwiftnessActive(player->GetGUID().GetCounter()))
                {
                    static const float burst[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                    rate += burst[Idx<uint8>(rank)];
                }
                float diff = unit->GetSpeedRate(MOVE_RUN) - rate;
                if (diff < 0.0f) diff = -diff;
                if (diff > 0.01f)
                    unit->SetSpeedRate(MOVE_RUN, rate);
            }
        }

        // One-time spawn stat application (Pack Tactics, Hardened Hide, etc.)
        if (g_petStatsApplied.count(unitGuid)) return;
        g_petStatsApplied.insert(unitGuid);
        ApplyPetStatAAs(unit, player);
    }

    // -----------------------------------------------------------------------
    // OnUnitDeath — remove unit GUID from applied-stats set so a re-summon
    // of the same pet/guardian gets a fresh stat application.
    // -----------------------------------------------------------------------
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        // Covers both Pet* (IsPet) and guardian Creature* (has player owner).
        Player* player = GetOwnerPlayer(unit);
        if (!player) return;

        uint32 unitGuid = unit->GetGUID().GetCounter();
        g_petStatsApplied.erase(unitGuid);
        g_sfIcd.erase(unitGuid);
        g_mendBondTick.erase(unitGuid);
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void AddSC_aa_pet()
{
    new aa_pet_unit();
}
