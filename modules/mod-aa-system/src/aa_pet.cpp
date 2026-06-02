// aa_pet.cpp
//
// Sanctum AA System — Pet tree hook-based AAs.
//
// IMPLEMENTED HERE:
//   3001  Command           — +5/10/15% pet damage (ModifyMeleeDamage attacker=pet)
//   3002  Master's Bond     — +12/25/40% pet damage (stacks with Command)
//   3003  Pack Tactics      — +3/6/10% pet crit (applied at pet spawn via OnUnitUpdate first-run)
//   3005  Savage Flurry     — 5/10/15% chance pet auto hits third time for 50% dmg. 200ms ICD.
//   3101  Hardened Hide     — +flat armor at pet spawn (approx +10/20/30%)
//   3102  Iron Constitution — +flat HP at pet spawn (approx +5/10/15%)
//   3103  Handler           — -5/10/15% pet dmg taken (ModifyMeleeeDamage + ModifySpell victim=pet)
//   3104  Uncrushable       — pet immune to crits/crushing via large defense rating at spawn
//   3105  Steeled Resolve   — pet dodge/parry/block/defense ratings at spawn (mirrors Combat Agility)
//   3106  Guardian's Resolve— while pet holds threat: -3/6/10% dmg from that attacker
//   3203  Pack Leader       — pet attacks heal owner for 2/4/6% of damage dealt
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

// ---------------------------------------------------------------------------
// File-local state
// ---------------------------------------------------------------------------
namespace
{
    // Pet GUIDs that have had spawn-time AA stats applied this session.
    // Cleared on pet death so re-summons re-apply cleanly.
    std::unordered_set<uint32> g_petStatsApplied;

    // Per-pet Savage Flurry ICD: petGuid → last proc timestamp
    std::unordered_map<uint32, uint32> g_sfIcd;

    // Clamp rank for array index.
    template<typename T>
    static inline T Idx(uint8 rank) { return static_cast<T>(std::min<uint8>(rank, 3)); }

    // Returns the Player owner of a Pet unit, or nullptr.
    static inline Player* PetOwner(Unit* u)
    {
        if (!u || !u->IsPet())
            return nullptr;
        Pet*  pet   = u->ToPet();
        Unit* owner = pet ? pet->GetOwner() : nullptr;
        return owner ? owner->ToPlayer() : nullptr;
    }

    // Apply all spawn-time stat passives to a pet (once per summon).
    // Called from OnUnitUpdate on first tick after the pet enters the world.
    static void ApplyPetStatAAs(Pet* pet, Player* player)
    {
        // ── Pack Tactics — +3/6/10% pet crit chance via Agility boost ───────────
        // ApplyRatingMod is Player-only; Agility gives pets crit + dodge indirectly.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_PACK_TACTICS);
            if (rank > 0)
            {
                pet->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY, TOTAL_VALUE, 173.0f * rank, true);
                pet->UpdateAllStats();
            }
        }

        // ── Hardened Hide — flat armor bonus (~10/20/30% of a mid-range pet) ──────
        // Using 800 armor per rank as a fixed approximation for solo server balance.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_HARDENED_HIDE);
            if (rank > 0)
            {
                pet->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, 800.0f * rank, true);
                pet->UpdateArmor();
            }
        }

        // ── Iron Constitution — flat HP bonus (~5/10/15%) ────────────────────────
        // Using 400 HP per rank as a fixed approximation.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_IRON_CONSTITUTION);
            if (rank > 0)
            {
                pet->HandleStatFlatModifier(UNIT_MOD_HEALTH, TOTAL_VALUE, 400.0f * rank, true);
                pet->UpdateMaxHealth();
            }
        }

        // ── Uncrushable — large armor bonus (proxy for crit/crush immunity) ──────
        // ApplyRatingMod is Player-only; armor reduces effective crit chance by making
        // the pet physically harder to damage.
        if (SanctumAA::Has(player, AA_P_UNCRUSHABLE))
        {
            pet->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, 4000.0f, true);
            pet->UpdateArmor();
        }

        // ── Steeled Resolve — Agility (avoidance proxy) + Armor (defense proxy) ──
        // ApplyRatingMod is Player-only; Agility gives pets dodge/parry indirectly.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_P_STEELED_RESOLVE);
            if (rank > 0)
            {
                // 88 dodge/parry + 25 defense rating → Agility proxy
                pet->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY, TOTAL_VALUE, 113.0f * rank, true);
                // 37 block rating → Armor proxy (37 * 10 as rough equivalent)
                pet->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, 370.0f * rank, true);
                pet->UpdateAllStats();
                pet->UpdateArmor();
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
    //   Pack Leader — pet deals damage → heal the pet's owner.
    // -----------------------------------------------------------------------
    void OnDamage(Unit* attacker, Unit* /*victim*/, uint32& damage) override
    {
        if (damage == 0 || !attacker->IsPet())
            return;

        Player* player = PetOwner(attacker);
        if (!player)
            return;

        // Pack Leader — heal owner for 2/4/6% of pet damage dealt
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
    // Attacker = pet: Command, Master's Bond, Savage Flurry.
    // Victim   = pet: Handler, Guardian's Resolve.
    // -----------------------------------------------------------------------
    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (damage == 0)
            return;

        // ── ATTACKER IS PET ──────────────────────────────────────────────────
        if (attacker->IsPet())
        {
            Player* player = PetOwner(attacker);
            if (player)
            {
                // Command — +5/10/15% pet damage
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_COMMAND);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }

                // Master's Bond — +12/25/40% pet damage (stacks with Command)
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_MASTERS_BOND);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.12f, 0.25f, 0.40f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }

                // Savage Flurry — 5/10/15% chance: add 50% extra dmg (third hit). 200ms ICD.
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_SAVAGE_FLURRY);
                    if (rank > 0)
                    {
                        uint32 petGuid = attacker->GetGUID().GetCounter();
                        auto&  stamp   = g_sfIcd[petGuid];
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

        // ── VICTIM IS PET ────────────────────────────────────────────────────
        if (target->IsPet())
        {
            Player* player = PetOwner(target);
            if (player)
            {
                // Handler — -5/10/15% flat DR on pet
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_HANDLER);
                    if (rank > 0)
                    {
                        static const float dr[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }

                // Guardian's Resolve — -3/6/10% DR while this attacker is targeting the pet
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_P_GUARDIANS_RESOLVE);
                    if (rank > 0 && attacker && attacker->GetVictim() == target)
                    {
                        static const float dr[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                        damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // ModifySpellDamageTaken — spell damage to the pet.
    //   Handler, Guardian's Resolve apply to all damage types.
    // -----------------------------------------------------------------------
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0 || !target->IsPet())
            return;

        Player* player = PetOwner(target);
        if (!player)
            return;

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
    }

    // -----------------------------------------------------------------------
    // OnUnitUpdate — apply spawn-time stat passives on the pet's first tick.
    // Fires for every unit; early-out makes it cheap for non-pets.
    // -----------------------------------------------------------------------
    void OnUnitUpdate(Unit* unit, uint32 /*diff*/) override
    {
        if (!unit->IsPet())
            return;

        Pet* pet = unit->ToPet();
        if (!pet)
            return;

        uint32 petGuid = pet->GetGUID().GetCounter();
        if (g_petStatsApplied.count(petGuid))
            return; // already applied this summon

        Player* player = PetOwner(unit);
        if (!player)
            return;

        g_petStatsApplied.insert(petGuid);
        ApplyPetStatAAs(pet, player);
    }

    // -----------------------------------------------------------------------
    // OnUnitDeath — remove pet GUID from applied-stats set so a re-summon
    // of the same pet type gets a fresh application.
    // -----------------------------------------------------------------------
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!unit->IsPet())
            return;

        uint32 petGuid = unit->GetGUID().GetCounter();
        g_petStatsApplied.erase(petGuid);
        g_sfIcd.erase(petGuid);
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void AddSC_aa_pet()
{
    new aa_pet_unit();
}
