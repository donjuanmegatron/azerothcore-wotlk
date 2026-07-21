// mod-aa-system.cpp
//
// Sanctum Alternate Advancement System
// -------------------------------------
// Three trees of permanent character progression:
//
//   General   — stat passives and advanced mechanics, open to all
//   Pet       — visible only to pet-owning classes
//   Archetype — role-based abilities (Tank / DPS / Healer)
//   Class     — deep class perks, one sub-tree per chosen class
//
// POINT ECONOMY
//   XP bleed:         Player-controlled 0-100% in steps of 10 (default 0%)
//   Command:          .aa bleed <0|10|20|...|100>
//   AA point rate:    1 point per (levelXp × 1.25 / 10) raw XP — scales per level
//   Rank costs:       R1=1pt, R2=2pts, R3=3pts (6pts to max a 3-rank AA)
//   Respec:           50g flat — resets all purchased AAs, refunds all points
//   Temper:           Auto-granted R1 for free at character creation
//
// PROGRESSION GATE
//   All AAs: level 60 minimum. No raid chain requirements.
//   Temper:  level 1 (free at creation).
//
// STAT EFFECTS (Phase 1 — direct stat passives):
//   2101 Iron Will         +200 HP per rank (flat proxy for ~1% HP)
//   2209 Sanctum Essence   +20 to all primary stats per rank
//   4105 Titan's Blood     +200 HP per rank (Tank archetype, class-gated in lore)
//   9001 Temper            No stat — enables .armoryslot temper command
//
// Phase 2+ effects (damage mods, procs, actives) are handled in
//   aa_combat_modifiers.cpp, aa_procs.cpp, aa_actives.cpp (pending build).
//
// COMMANDS
//   .aa              — show AA points available/spent
//   .aa list         — show all purchased AAs and their ranks
//   .aa buy <id>     — purchase next rank of an AA
//   .aa respec       — respec all AAs for 50g
//   .aa grant <id> <rank>   — GM: set AA rank on selected player
//   .aa addpoints <amount>  — GM: directly add AA points

#include "aa_runtime.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "CommandScript.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Log.h"
#include "Unit.h"
#include "SharedDefines.h"
#include "Creature.h"
#include "Item.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include <unordered_map>
#include <map>
#include <sstream>

// Cross-module API from mod-gear-tiers.cpp (same binary)
extern void GearTiers_AddGXP(Player* player, uint32 amount);

// Cross-module API from aa_actives.cpp (same binary)
extern bool SanctumAA_HandleActivate(Player* player, uint32 aaId, ChatHandler* handler);
extern void SanctumAA_ClearActivateState(uint32 guid);

using namespace Acore::ChatCommands;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr uint32 AA_RESPEC_COST  = 500000;  // 50g in copper
static constexpr uint8  AA_MAX_RANK     = 5;        // hard cap; per-AA limits enforced by client

// ---------------------------------------------------------------------------
// Per-character runtime data
// ---------------------------------------------------------------------------
struct AaData
{
    uint64 aaXp         = 0;
    uint32 pointsEarned = 0;
    uint32 pointsSpent  = 0;
    uint8  bleedPct     = 0;    // player-controlled: 0-100 in steps of 10
    uint8  classes[3]   = {0, 0, 0};  // multiclass class ids (character_multiclass), cached at login

    std::map<uint32, uint8> purchased;  // aa_id → current_rank

    uint32 PointsAvailable() const
    {
        return pointsEarned > pointsSpent ? (pointsEarned - pointsSpent) : 0;
    }
};

static std::unordered_map<uint32, AaData> s_aaData;  // key = player GUID counter

// Sends a data packet directly to the SanctumAA addon.
// Uses ||SA|| prefix so the Lua filter catches it 100% of the time and it
// never leaks into the visible chat window, even with other addons installed.
static void SendToAAAddon(Player* player, const std::string& msg)
{
    std::string fullMsg = "||SA||" + msg;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL,
        nullptr, nullptr, fullMsg);
    player->GetSession()->SendPacket(&data);
}

// ---------------------------------------------------------------------------
// SanctumAA::GetRank — declared in aa_runtime.h, defined here because
// s_aaData lives in this translation unit.
// ---------------------------------------------------------------------------
uint8 SanctumAA::GetRank(Player const* player, uint32 aaId)
{
    if (!player)
        return 0;
    auto it = s_aaData.find(player->GetGUID().GetCounter());
    if (it == s_aaData.end())
        return 0;
    auto jt = it->second.purchased.find(aaId);
    return (jt != it->second.purchased.end()) ? jt->second : 0;
}

// SanctumAA::PlayerHasClass — true if classId is one of the player's three
// multiclass classes (cached at login). Used to class-gate combat-hook AAs so a
// wrong-class holder (e.g. via .aa testall) doesn't trigger a class-only effect.
bool SanctumAA::PlayerHasClass(Player const* player, uint8 classId)
{
    if (!player)
        return false;
    auto it = s_aaData.find(player->GetGUID().GetCounter());
    if (it == s_aaData.end())
        return false;
    AaData const& d = it->second;
    return d.classes[0] == classId || d.classes[1] == classId || d.classes[2] == classId;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static AaData& GetAaData(uint32 guid)  { return s_aaData[guid]; }
static void    RemoveAaData(uint32 guid) { s_aaData.erase(guid); }

// ---------------------------------------------------------------------------
// Tier gate — minimum level required to purchase an AA.
// Master list: all AAs available at level 60. Temper: free at level 1.
// ---------------------------------------------------------------------------
static uint8 GetAATierLevel(uint32 aaId)
{
    if (aaId == AA_TEMPER)
        return 1;
    return 60;
}

// ---------------------------------------------------------------------------
// Per-ability max rank and rank cost tables
// ---------------------------------------------------------------------------
static uint8 GetAAMaxRank(uint32 aaId)
{
    static const std::unordered_map<uint32, uint8> s_max = {
        // maxRank 1
        {2019,1},{2110,1},{2112,1},{2113,1},{2204,1},
        {3104,1},{3201,1},{3202,1},{3204,1},
        {4104,1},{4204,1},{4304,1},
        {5002,1},
        // 5018 Unending Fury — SCRAPPED (Tier 1)
        {5207,1},{5208,1},{5218,1},{5236,1},
        {5303,1},{5312,1},{5321,1},{5328,1},
        {5404,1},
        // 5424 Sanctification — SCRAPPED (Tier 1)
        // 5426 Wake of Tranquility — SCRAPPED (Tier 1)
        // 5435 Disciple of C'Thun — SCRAPPED (Tier 1)
        // 5436 Wandering Spirits — SCRAPPED (Tier 1)
        // 5442 Improved Lightwell — SCRAPPED (Tier 1)
        {5518,1},{5519,1},{5520,1},{5522,1},
        {5731,1},
        {5828,1},
        // 5800 Threads of Despair    — SCRAPPED (Tier 1)
        // 5821 Well of Souls         — SCRAPPED (Tier 1)
        // 5822 Soul Barrage          — SCRAPPED (Tier 1)
        // 5823 Soulstorm             — SCRAPPED (Tier 1)
        // 5824 Circle of the Damned  — SCRAPPED (Tier 1)
        // 5825 Soul Mirror           — SCRAPPED (Tier 1)
        // 5826 Wake the Dead         — SCRAPPED (Tier 1)
        // 5829 Dire Charm            — SCRAPPED (Tier 1)
        // 5833 Suspended Minion      — SCRAPPED (Tier 1)
        // 5834 Feigned Minion        — SCRAPPED (Tier 1)
        // 5835 Spell Casting Subtlety — SCRAPPED (Tier 1)
        // Druid one-shots
        {5925,1},  // Call of the Wild (renamed from Dire Charm)
        {9001,1},
        // maxRank 2
        {5009,2},{5011,2},{5014,2},{5015,2},
        // 5016 Titan's Grip — SCRAPPED (Tier 1)
        // maxRank 4
        {5012,4},{4103,4},
    };
    auto it = s_max.find(aaId);
    return it != s_max.end() ? it->second : 3;
}

static uint8 GetAARankCost(uint32 aaId, uint8 nextRank)
{
    static const std::unordered_map<uint32, std::array<uint8,4>> s_costs = {
        {9001,{0,0,0,0}},{2019,{5,0,0,0}},{2110,{5,0,0,0}},{2121,{2,3,4,0}},
        {2112,{15,0,0,0}},
        {2204,{5,0,0,0}},{3002,{2,4,6,0}},{3104,{5,0,0,0}},{3105,{2,3,4,0}},
        {3201,{5,0,0,0}},{3202,{5,0,0,0}},{3204,{3,0,0,0}},{3205,{2,3,4,0}},
        {4103,{1,2,3,4}},
        {4104,{5,0,0,0}},{4106,{2,3,4,0}},{4202,{2,3,4,0}},{4204,{5,0,0,0}},
        {4205,{3,5,8,0}},{4206,{2,3,4,0}},{4304,{4,0,0,0}},
        {5002,{4,0,0,0}},{5007,{2,3,4,0}},{5009,{1,2,0,0}},{5011,{2,3,0,0}},
        {5012,{1,2,3,4}},{5014,{2,4,0,0}},{5015,{1,2,0,0}},
        // 5016 Titan's Grip — SCRAPPED (Tier 1); 5018 Unending Fury — SCRAPPED (Tier 1)
        // 5008 Cleaving Strikes — SCRAPPED (Tier 1) (used default 1/2/3 cost; now excluded)
        {5204,{2,3,4,0}},{5207,{5,0,0,0}},{5208,{4,0,0,0}},
        {5218,{3,0,0,0}},{5236,{4,0,0,0}},{5240,{2,3,4,0}},{5246,{2,3,4,0}},
        {5303,{5,0,0,0}},{5312,{3,0,0,0}},{5314,{2,3,4,0}},{5317,{2,3,4,0}},
        {5321,{3,0,0,0}},{5328,{4,0,0,0}},{5333,{2,3,4,0}},{5340,{2,3,4,0}},
        // 2113 Cleanse Curse (General-Defensive active)
        {2113,{5,0,0,0}},
        // Priest — non-default costs
        {5401,{2,3,4,0}},  // Twinheal R1=2 R2=3 R3=4
        {5403,{2,3,4,0}},  // Channeling the Divine R1=2 R2=3 R3=4
        {5404,{5,0,0,0}},  // Forceful Rejuvenation ONE-SHOT 5pt
        {5412,{2,3,4,0}},  // Force of Will R1=2 R2=3 R3=4
        {5421,{2,3,4,0}},  // Bestow Divine Aura R1=2 R2=3 R3=4
        {5431,{2,3,4,0}},  // Harbinger R1=2 R2=3 R3=4
        // 5424/5426/5435/5436/5442 SCRAPPED — no entry needed
        {5443,{2,3,4,0}},  // Divine Purpose R1=2 R2=3 R3=4{5514,{2,3,4,0}},{5518,{5,0,0,0}},{5519,{5,0,0,0}},
        {5520,{4,0,0,0}},{5522,{4,0,0,0}},{5621,{3,5,8,0}},{5704,{2,3,4,0}},
        {5731,{5,0,0,0}},{5733,{2,3,4,0}},{5738,{2,3,4,0}},
        // Mage new AAs 5741-5746
        {5741,{1,2,3,0}},{5742,{1,2,3,0}},{5743,{2,3,4,0}},
        {5744,{1,2,3,0}},{5745,{1,2,3,0}},{5746,{1,2,3,0}},
        {5816,{2,3,4,0}},{5817,{2,3,4,0}},
        {5828,{5,0,0,0}},
        // 5800,5821,5822,5823,5824,5825,5826,5829,5833,5834,5835 — SCRAPPED (Tier 1)
        // Warrior Vengeful Bulwark (5019) — default 1/2/3 cost
        // DK Corrupted Carapace (5527) — default 1/2/3 cost
        // Druid non-default costs
        {5917,{2,3,4,0}},  // Spirit of the Wood R1=2 R2=3 R3=4
        {5925,{4,0,0,0}},  // Call of the Wild one-shot 4pt
        {5927,{2,3,4,0}},  // Stampeding Roar R1=2 R2=3 R3=4
        {5930,{2,3,4,0}},  // Survival Instincts R1=2 R2=3 R3=4
        {5931,{2,3,4,0}},  // Ironfur (burn-tank) R1=2 R2=3 R3=4
    };
    auto it = s_costs.find(aaId);
    if (it != s_costs.end())
    {
        if (nextRank >= 1 && nextRank <= 4)
            return it->second[nextRank - 1];
        return 99;
    }
    return (uint8)nextRank;  // default: R1=1pt, R2=2pt, R3=3pt
}

// ---------------------------------------------------------------------------
// Stat application
// ---------------------------------------------------------------------------
// Phase 1 — direct stat passives.
// All other AAs are tracked in the DB but have no stat effect yet;
// their effects are implemented in aa_combat_modifiers.cpp (Phase 2+).
//
// Called:
//   Login   — rankDelta = full purchased rank (apply all at once)
//   Buy     — rankDelta = 1                   (apply just the new rank)
//   Respec  — rankDelta = full purchased rank, apply=false (remove all)
// ---------------------------------------------------------------------------
static void ApplyAAStat(Player* player, uint32 aaId, uint8 rankDelta, bool apply)
{
    if (rankDelta == 0)
        return;

    switch (aaId)
    {
        case AA_G_CRITICAL_MASS:    // +1/2/3% crit chance (melee, ranged, spell)
        {
            // ~45 crit rating = 1% crit at level 80 (WotLK DBC scale).
            // Gives slightly more crit at lower levels — acceptable for solo server.
            int32 amount = 45 * (int32)rankDelta;
            player->ApplyRatingMod(CR_CRIT_MELEE,  amount, apply);
            player->ApplyRatingMod(CR_CRIT_RANGED, amount, apply);
            player->ApplyRatingMod(CR_CRIT_SPELL,  amount, apply);
            break;
        }
        case AA_G_IRON_WILL:        // +200 HP per rank
        {
            float bonus = 200.0f * rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_HEALTH, TOTAL_VALUE, bonus, apply);
            player->UpdateMaxHealth();
            break;
        }
        case AA_G_INDOMITABLE:      // ONE-SHOT: permanent immunity to ALL crowd control
        {
            // Apply mechanic immunity for every loss-of-control effect. Re-applied
            // on login via ApplyAllAAStats, removed on respec (apply == false).
            static const uint32 ccMechanics[] = {
                MECHANIC_CHARM, MECHANIC_DISORIENTED, MECHANIC_FEAR, MECHANIC_ROOT,
                MECHANIC_SILENCE, MECHANIC_SLEEP, MECHANIC_STUN, MECHANIC_FREEZE,
                MECHANIC_KNOCKOUT, MECHANIC_POLYMORPH, MECHANIC_BANISH, MECHANIC_SHACKLE,
                MECHANIC_HORROR, MECHANIC_TURN, MECHANIC_SAPPED, MECHANIC_DAZE,
                MECHANIC_SNARE
            };
            for (uint32 m : ccMechanics)
                player->ApplySpellImmune(0, IMMUNITY_MECHANIC, m, apply);

            // Mechanic immunity alone MISSES crowd control that lands via the aura
            // type without a CC mechanic flag (e.g. ZG "Whirling Trip" 24048 stuns
            // with no MECHANIC_STUN). Also grant IMMUNITY_STATE for every loss-of-
            // control aura type so ALL stuns/fears/roots/etc. are blocked regardless
            // of how the spell is defined.
            static const uint32 ccAuraTypes[] = {
                SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_CONFUSE,
                SPELL_AURA_MOD_ROOT, SPELL_AURA_MOD_SILENCE, SPELL_AURA_MOD_CHARM,
                SPELL_AURA_MOD_POSSESS, SPELL_AURA_MOD_PACIFY, SPELL_AURA_MOD_PACIFY_SILENCE,
                SPELL_AURA_MOD_DECREASE_SPEED, SPELL_AURA_TRANSFORM
            };
            for (uint32 a : ccAuraTypes)
                player->ApplySpellImmune(0, IMMUNITY_STATE, a, apply);
            break;
        }
        case AA_G_SANCTUM_ESSENCE:  // +20 to all primary stats per rank
        {
            float bonus = 20.0f * rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  TOTAL_VALUE, bonus, apply);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   TOTAL_VALUE, bonus, apply);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   TOTAL_VALUE, bonus, apply);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, bonus, apply);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    TOTAL_VALUE, bonus, apply);
            player->UpdateAllStats();
            break;
        }
        case AA_T_TITANS_BLOOD:     // +200 HP per rank (Tank archetype)
        {
            float bonus = 200.0f * rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_HEALTH, TOTAL_VALUE, bonus, apply);
            player->UpdateMaxHealth();
            break;
        }
        case AA_T_STALWART:         // +50/100/150 flat block value per rank
        {
            player->HandleBaseModFlatValue(SHIELD_BLOCK_VALUE, 50.0f * rankDelta, apply);
            break;
        }
        case AA_H_CRITICAL_HEALING: // +3%/+6%/+10% heal crit (~150 spell crit rating per rank)
        {
            player->ApplyRatingMod(CR_CRIT_SPELL, 150 * (int32)rankDelta, apply);
            break;
        }
        case AA_H_SPIRIT_CHANNEL:   // +50/100/150 flat Spirit per rank
        {
            float bonus = 50.0f * rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT, TOTAL_VALUE, bonus, apply);
            player->UpdateAllStats();
            break;
        }
        case AA_G_COMBAT_AGILITY:
        {
            // ~88 dodge/parry rating ≈ 2.3% at L80; 37 block ≈ 2.3%; 25 defense per rank
            // Targets: +2/4/7% dodge/parry/block, +20/45/75 defense (linear approx hits R3 exactly)
            int32 avoidance = 88 * (int32)rankDelta;
            int32 block     = 37 * (int32)rankDelta;
            int32 defense   = 25 * (int32)rankDelta;
            player->ApplyRatingMod(CR_DODGE,         avoidance, apply);
            player->ApplyRatingMod(CR_PARRY,         avoidance, apply);
            player->ApplyRatingMod(CR_BLOCK,         block,     apply);
            player->ApplyRatingMod(CR_DEFENSE_SKILL, defense,   apply);
            break;
        }
        case AA_WAR_TACTICAL_MASTERY:   // +84 armor pen rating per rank (~3% ArP at L80)
        {
            player->ApplyRatingMod(CR_ARMOR_PENETRATION, 84 * (int32)rankDelta, apply);
            break;
        }
        case AA_ROG_HASTENED_ATTACKS:   // +164 melee haste rating per rank (~1% haste at L80)
        {
            player->ApplyRatingMod(CR_HASTE_MELEE, 164 * (int32)rankDelta, apply);
            break;
        }
        case AA_HUN_NATURES_GUIDANCE:   // +16 ranged hit rating per rank (~0.5% hit at L80)
        {
            player->ApplyRatingMod(CR_HIT_RANGED, 16 * (int32)rankDelta, apply);
            break;
        }
        case AA_HUN_STEADY_FOCUS:       // +60 ranged attack power per rank delta (≈60/120/180 at R1/R2/R3)
        {
            // Uses the same additive-per-rank-delta pattern as Iron Will (+200/delta) and
            // Sanctum Essence (+20/delta). Each rank purchased or applied on login adds 60 AP.
            // Cumulative at R3 = 180 AP (design target was 200; the 20 AP difference is acceptable
            // for a solo server and keeps the implementation identical to every other stat passive).
            float bonus = 60.0f * (float)rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER_RANGED, TOTAL_VALUE, bonus, apply);
            player->UpdateAttackPowerAndDamage(true);
            break;
        }
        case AA_HUN_PIERCING_ROUNDS:    // +84 armor pen rating per rank (~3% ArP at L80, same as Tactical Mastery)
        {
            player->ApplyRatingMod(CR_ARMOR_PENETRATION, 84 * (int32)rankDelta, apply);
            break;
        }
        case AA_HUN_SURVIVAL_TACTICS:   // +dodge rating per rank (~2/4/7% at L80)
        {
            // 45/90/160 dodge rating cumulative: applying 45 per rank-delta matches the additive step.
            player->ApplyRatingMod(CR_DODGE, 45 * (int32)rankDelta, apply);
            break;
        }
        case AA_G_MANA_SURGE:   // +2/4/6% mana regen — Spirit proxy (+40/80/120 Spirit per rank)
        {
            // No direct %-mana-regen stat in 3.3.5a. Spirit drives mana regen via the
            // spirit coefficient formula on all casting classes. +40 Spirit ≈ +50 MP5 at L80.
            float bonus = 40.0f * rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT, TOTAL_VALUE, bonus, apply);
            player->UpdateAllStats();
            break;
        }
        case AA_ROG_AMBIDEXTERITY:      // +8%/16%/25% off-hand hit (≈30 melee hit rating per rank)
        {
            // Off-hand hit proxy: +30 melee hit rating per rank delta.
            // ~1% hit at L80 per 32.8 hit rating; 30 ≈ ~0.9% per rank, ×3 ranks ≈ +2.7% hit total.
            // The off-hand damage reduction is handled in aa_class.cpp ModifyMeleeDamage.
            player->ApplyRatingMod(CR_HIT_MELEE, 30 * (int32)rankDelta, apply);
            break;
        }
        case AA_SHA_WINDLORD:           // +10/20% Attack Power (R1=+10%, R2=+20%, R3=+20%+extra-attacks via hook)
        {
            // Each rankDelta step adds 10% of base AP as a flat bonus.
            // R1: +10%, R2: +20%, R3: +20% AP + 3 extra attacks (extra attacks handled in aa_class.cpp OnPlayerSpellCast).
            // Approximation: 300 flat AP per rank delta is a reasonable midrange bonus; on-equip recalc covers scaling.
            float bonus = 300.0f * (float)rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE, bonus, apply);
            player->UpdateAttackPowerAndDamage(false);
            break;
        }
        case AA_HUN_ENDLESS_QUIVER:     // +5%/+10%/+15% ranged attack speed (164 rating ≈ 5% at L80)
        {
            player->ApplyRatingMod(CR_HASTE_RANGED, 164 * (int32)rankDelta, apply);
            break;
        }
        case AA_PRI_ARMOR_OF_WISDOM:    // +0.333 armor per Int per rank (snapshot — total 1.0 at R3)
        {
            float intVal    = (float)player->GetStat(STAT_INTELLECT);
            float bonus     = intVal * 0.333f * (float)rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_ARMOR, TOTAL_VALUE, bonus, apply);
            player->UpdateAllStats();
            break;
        }
        case AA_PRI_SPIRITUAL_LIGHT:    // 10% of Spirit as bonus Spirit per rank (snapshot)
        {
            float spiritVal = (float)player->GetStat(STAT_SPIRIT);
            float bonus     = spiritVal * 0.10f * (float)rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT, TOTAL_VALUE, bonus, apply);
            player->UpdateAllStats();
            break;
        }
        case AA_MAG_IMPROVED_FAMILIAR:  // +3/5/8% SP (via Int) + 5/8/12% max mana per rank delta
        {
            // Spec (2026-06-25 pass): +3/5/8% SP implemented as flat Int bonus (Int drives SP on casters).
            // +5/8/12% max mana as flat mana bonus (snapshot on apply/login).
            // Per rankDelta step: approximately +5% mana and +45 Int each delta step.
            float intBonus  = 45.0f * (float)rankDelta;   // each rank step ~+45 Int (~+45 SP at cap)
            float manaBonus = (float)player->GetMaxPower(POWER_MANA) * 0.05f * (float)rankDelta;
            player->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, intBonus, apply);
            player->HandleStatFlatModifier(UNIT_MOD_MANA, TOTAL_VALUE, manaBonus, apply);
            player->UpdateAllStats();
            break;
        }

        case AA_MAG_SPELL_CASTING_SUBTLETY:  // -10/20/30% all spell threat
        {
            // Threat reduction: use ApplySpellMod with SPELLMOD_THREAT (approximation).
            // In 3.3.5a there is no clean "all spell threat -X%" stat mod. We use a threat
            // coefficient via the existing ThreatManager (no direct player stat hook).
            // PARTIAL: stored in DB; threat hooks are not easily available in AC 3.3.5a.
            // No ApplyAAStat effect; handled as best-effort in the future if threat hooks are exposed.
            break;
        }
        case AA_TEMPER:
            // No stat effect — this AA enables the .armoryslot temper command
            break;

        // ── Druid: Healing Gift (5919) — +3/6/10% heal crit (Tree form gated; rating mod) ──
        // Using 150 crit rating per rank as proxy for +3%/+6%/+10% at L80.
        // Gate enforcement (Tree form only) is in aa_combat_modifiers.cpp ModifyHealReceived.
        // We apply the rating unconditionally here as a floor — the hook gates it.
        case AA_DRU_HEALING_GIFT:
        {
            player->ApplyRatingMod(CR_CRIT_SPELL, 150 * (int32)rankDelta, apply);
            break;
        }

        // ── Druid: Improved Berserk passives handled fully in aa_combat_modifiers.cpp ──
        // ── Vengeful Bulwark (5019), Corrupted Carapace (5527) — no stat; combat hook only ──
        case AA_WAR_VENGEFUL_BULWARK:
        case AA_DK_CORRUPTED_CARAPACE:
            // No stat effect — effects in aa_combat_modifiers.cpp
            break;

        // ── Druid: Wrath of the Wild (5907) — absorb ward; initialized on buy/login ──
        // The absorb is managed fully in aa_combat_modifiers.cpp OnUnitUpdate; no stat here.
        case AA_DRU_WRATH_OF_THE_WILD:
            break;

        default:
            // Phase 2+ AAs — tracked in DB, effects implemented in aa_combat_modifiers.cpp
            break;
    }
}

static void ApplyAllAAStats(Player* player)
{
    AaData& data = GetAaData(player->GetGUID().GetCounter());
    for (auto const& [aaId, rank] : data.purchased)
        ApplyAAStat(player, aaId, rank, true);
}

static void RemoveAllAAStats(Player* player)
{
    AaData& data = GetAaData(player->GetGUID().GetCounter());
    for (auto const& [aaId, rank] : data.purchased)
        ApplyAAStat(player, aaId, rank, false);
}

// ---------------------------------------------------------------------------
// Database I/O
// ---------------------------------------------------------------------------
static void LoadAAData(Player* player)
{
    uint32 guid = player->GetGUID().GetCounter();
    AaData& data = GetAaData(guid);
    data = AaData{};

    QueryResult result = CharacterDatabase.Query(
        "SELECT aa_xp, points_earned, points_spent, aa_bleed_pct FROM character_aa_points WHERE guid = {}", guid);
    if (result)
    {
        Field* f      = result->Fetch();
        data.aaXp         = f[0].Get<uint64>();
        data.pointsEarned = f[1].Get<uint32>();
        data.pointsSpent  = f[2].Get<uint32>();
        data.bleedPct     = f[3].Get<uint8>();

        // Migrate from old system where aa_xp was a lifetime total (1 pt per 1000 XP).
        // The new system stores only XP toward the next point (resets on each grant).
        // Old progress-within-point was aa_xp % 1000 (max 999); discard the rest.
        if (data.aaXp >= 1000)
            data.aaXp %= 1000;
    }

    QueryResult purchased = CharacterDatabase.Query(
        "SELECT aa_id, aa_rank FROM character_aa_purchased WHERE guid = {}", guid);
    if (purchased)
    {
        do
        {
            Field* f  = purchased->Fetch();
            data.purchased[f[0].Get<uint32>()] = f[1].Get<uint8>();
        } while (purchased->NextRow());
    }

    // Cache the player's three multiclass class ids so combat hooks can class-gate
    // AAs without a per-hit DB query (e.g. Vengeful Bulwark = Warrior-only).
    QueryResult classResult = CharacterDatabase.Query(
        "SELECT class1, class2, class3 FROM character_multiclass WHERE guid = {}", guid);
    if (classResult)
    {
        Field* cf = classResult->Fetch();
        data.classes[0] = cf[0].Get<uint8>();
        data.classes[1] = cf[1].Get<uint8>();
        data.classes[2] = cf[2].Get<uint8>();
    }
}

static void SaveAAPoints(Player* player)
{
    uint32 guid = player->GetGUID().GetCounter();
    AaData const& data = GetAaData(guid);
    CharacterDatabase.Execute(
        "REPLACE INTO character_aa_points (guid, aa_xp, points_earned, points_spent, aa_bleed_pct) "
        "VALUES ({}, {}, {}, {}, {})",
        guid, data.aaXp, data.pointsEarned, data.pointsSpent, (uint32)data.bleedPct);
}

static void SavePurchasedAA(Player* player, uint32 aaId, uint8 rank)
{
    uint32 guid = player->GetGUID().GetCounter();
    CharacterDatabase.Execute(
        "REPLACE INTO character_aa_purchased (guid, aa_id, aa_rank) VALUES ({}, {}, {})",
        guid, aaId, rank);
}

static void DeleteAllPurchasedAA(Player* player)
{
    uint32 guid = player->GetGUID().GetCounter();
    CharacterDatabase.Execute("DELETE FROM character_aa_purchased WHERE guid = {}", guid);
}

// Forward declaration — defined before AwardAAXP below
static uint32 ComputeAAThreshold(Player* player);

// ---------------------------------------------------------------------------
// Client sync — push AA state to the SanctumAA addon
//
// Protocol:
//   SANCTUMAA:INIT:<earned>:<spent>
//   SANCTUMAA:CLASSES:<c1>:<c2>:<c3>
//   SANCTUMAA:AA:<id>:<rank>           (one per purchased AA)
//   SANCTUMAA:READY
//   SANCTUMAA:BOUGHT:<id>:<rank>:<earned>:<spent>
//   SANCTUMAA:RESPEC:<earned>
// ---------------------------------------------------------------------------
static void PushAADataToClient(Player* player)
{
    uint32 guid = player->GetGUID().GetCounter();
    AaData const& data = GetAaData(guid);
    ChatHandler ch(player->GetSession());

    uint32 threshold = ComputeAAThreshold(player);
    SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:INIT:{}:{}:{}:{}:{}",
        data.pointsEarned, data.pointsSpent, (uint32)data.aaXp,
        (uint32)data.bleedPct, threshold));

    QueryResult classResult = CharacterDatabase.Query(
        "SELECT class1, class2, class3 FROM character_multiclass WHERE guid = {}", guid);
    if (classResult)
    {
        Field* f = classResult->Fetch();
        SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:CLASSES:{}:{}:{}",
            (uint32)f[0].Get<uint8>(), (uint32)f[1].Get<uint8>(), (uint32)f[2].Get<uint8>()));
    }
    else
    {
        SendToAAAddon(player, "SANCTUMAA:CLASSES:0:0:0");
    }

    for (auto const& [aaId, rank] : data.purchased)
        SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:AA:{}:{}", aaId, (uint32)rank));

    SendToAAAddon(player, "SANCTUMAA:READY");
}

// ---------------------------------------------------------------------------
// AA XP threshold — scales with the player's current level.
// Earning 10 AA points requires 1.25× the XP for one full level at that level.
// At level 80 (cap), uses the level 79→80 XP requirement as the reference.
// ---------------------------------------------------------------------------
static uint32 ComputeAAThreshold(Player* player)
{
    static constexpr float  LEVEL_XP_MULT    = 1.25f;
    static constexpr uint32 POINTS_PER_LEVEL = 10;

    uint8  lvl     = player->GetLevel();
    // Cap lookup at level 59 — WotLK inflates XP per level by 10x at level 60+
    // (TBC/WotLK zone transitions), which would make thresholds absurdly large.
    // Vanilla levels top out around 220K XP/level, giving ~27K threshold per point.
    uint8  lookupLvl = lvl < 60 ? lvl : 59;
    uint32 levelXp   = sObjectMgr->GetXPForLevel(lookupLvl);
    uint32 threshold = static_cast<uint32>(levelXp * LEVEL_XP_MULT / POINTS_PER_LEVEL);
    return threshold < 500u ? 500u : threshold;
}

// ---------------------------------------------------------------------------
// AA XP award
// ---------------------------------------------------------------------------
static void AwardAAXP(Player* player, uint64 xpAmount)
{
    if (xpAmount == 0)
        return;

    uint32 guid = player->GetGUID().GetCounter();
    AaData& data = GetAaData(guid);
    data.aaXp += xpAmount;

    uint32 threshold = ComputeAAThreshold(player);
    uint32 gained    = 0;
    while (data.aaXp >= threshold)
    {
        data.aaXp -= threshold;
        data.pointsEarned++;
        gained++;
    }

    if (gained > 0)
    {
        std::ostringstream msg;
        msg << "|cff00ccff[AA]|r +" << gained << " point"
            << (gained != 1 ? "s" : "") << " earned! ("
            << data.PointsAvailable() << " available)";
        ChatHandler ch(player->GetSession());
        ch.SendSysMessage(msg.str().c_str());
        // NOTE: do NOT send SANCTUMAA:INIT here. INIT is the heavyweight
        // "wipe + rebuild" message: the client clears aaPurchased (and its saved
        // copy) and rebuilds the ability bar from an empty list — which HID the
        // Sanctum Ability Bar mid-combat every time a kill crossed a point
        // threshold, and dropped the client's purchased-AA list until the next
        // relog/`.aa sync`. The lightweight SANCTUMAA:XP message below already
        // carries the updated pointsEarned + threshold, so the point-up shows up
        // in the UI without touching the purchased list or the bar.
    }

    // Live progress update for the AA UI bar on EVERY award (not just on a
    // point-up). Lightweight — updates the XP bar + points without rebuilding
    // the purchased-ability list the way INIT does.
    SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:XP:{}:{}:{}",
        (uint32)data.aaXp, data.pointsEarned, threshold));

    SaveAAPoints(player);
}

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
static const char* GetAAName(uint32 aaId)
{
    switch (aaId)
    {
        // General — Offensive
        case AA_G_DOUBLE_STRIKE:    return "Double Strike";
        case AA_G_PRECISION:        return "Precision";
        case AA_G_CRITICAL_MASS:    return "Critical Mass";
        case AA_G_KILLING_BLOW:     return "Killing Blow";
        case AA_G_VENGEANCE:        return "Vengeance";
        case AA_G_ATTENTION:        return "Attention";
        case AA_G_INSPIRATION:      return "Inspiration";
        case AA_G_SAVAGERY:         return "Savagery";
        case AA_G_FEROCITY:         return "Ferocity";
        case AA_G_THOUSAND_CUTS:    return "Thousand Cuts";
        case AA_G_SCHOOL_FIRE:      return "School Mastery: Fire";
        case AA_G_SCHOOL_FROST:     return "School Mastery: Frost";
        case AA_G_SCHOOL_SHADOW:    return "School Mastery: Shadow";
        case AA_G_SCHOOL_HOLY:      return "School Mastery: Holy";
        case AA_G_SCHOOL_NATURE:    return "School Mastery: Nature";
        case AA_G_SCHOOL_ARCANE:    return "School Mastery: Arcane";
        case AA_G_SCHOOL_PHYSICAL:  return "School Mastery: Physical";
        case AA_G_OUTBURST:         return "Outburst";
        case AA_G_BERSERKERS_EDGE:  return "Berserker's Edge";
        // General — Defensive
        case AA_G_IRON_WILL:        return "Iron Will";
        case AA_G_THICK_HIDE:       return "Thick Hide";
        case AA_G_WARDING:          return "Warding";
        case AA_G_NATURAL_RENEWAL:  return "Natural Renewal";
        case AA_G_REANIMATION:      return "Reanimation";
        case AA_G_VITALITY:         return "Vitality";
        case AA_G_HARDENING:        return "Hardening";
        case AA_G_BULWARK:          return "Bulwark";
        case AA_G_HINDSIGHT:        return "Hindsight";
        case AA_G_FREE_WILL:        return "Free Will";
        case AA_G_RECOVERY:         return "Recovery";
        case AA_G_INDOMITABLE:      return "Indomitable";
        case AA_G_COMBAT_AGILITY:   return "Combat Agility";
        case AA_G_CHANNELING_FOCUS: return "Channeling Focus";
        // General — Utility
        case AA_G_SPRINTER:         return "Sprinter";
        case AA_G_MANA_SURGE:       return "Mana Surge";
        case AA_G_VITAL_HUNGER:     return "Vital Hunger";
        case AA_G_ENDURING_RITES:   return "Enduring Rites";
        case AA_G_ZEAL:             return "Zeal";
        case AA_G_TEMPERED_BODY:    return "Tempered Body";
        case AA_G_LUCKY_FIND:       return "Lucky Find";
        case AA_G_CAUTION:          return "Caution";
        case AA_G_SANCTUM_ESSENCE:  return "Sanctum Essence";
        // Pet — Offense
        case AA_P_COMMAND:          return "Command";
        case AA_P_MASTERS_BOND:     return "Master's Bond";
        case AA_P_PACK_TACTICS:     return "Pack Tactics";
        case AA_P_PREDATORS_HOWL:   return "Predator's Howl";
        case AA_P_SAVAGE_FLURRY:    return "Savage Flurry";
        case AA_P_BLOODSCENT:       return "Bloodscent";
        // Pet — Defense
        case AA_P_HARDENED_HIDE:     return "Hardened Hide";
        case AA_P_IRON_CONSTITUTION: return "Iron Constitution (Pet)";
        case AA_P_HANDLER:           return "Handler";
        case AA_P_UNCRUSHABLE:       return "Uncrushable";
        case AA_P_STEELED_RESOLVE:   return "Steeled Resolve";
        case AA_P_GUARDIANS_RESOLVE: return "Guardian's Resolve";
        case AA_P_MENDING_BOND:      return "Mending Bond";
        // Pet — Utility
        case AA_P_ASSIST_ME:        return "Assist Me";
        case AA_P_REDIRECTION:      return "Redirection";
        case AA_P_PACK_LEADER:      return "Pack Leader";
        case AA_P_SOUL_BOND:        return "Soul Bond";
        case AA_P_STAT_INHERITANCE: return "Stat Inheritance Boost";
        // Archetype — Tank
        case AA_T_STALWART:         return "Stalwart";
        case AA_T_IRON_RESOLVE:     return "Iron Resolve";
        case AA_T_ANCHORED:         return "Anchored";
        case AA_T_LAST_STAND:       return "Last Stand";
        case AA_T_TITANS_BLOOD:     return "Titan's Blood";
        case AA_T_DOUBLE_RIPOSTE:   return "Double Riposte";
        // Archetype — DPS
        case AA_D_BLOODLETTING:     return "Bloodletting";
        case AA_D_HASTE_SURGE:      return "Haste Surge";
        case AA_D_MORTAL_STRIKE:    return "Mortal Strike";
        case AA_D_APEX_PREDATOR:    return "Apex Predator";
        case AA_D_WEAPON_FURY:      return "Weapon Fury";
        case AA_D_TWINCAST:         return "Twincast";
        // Archetype — Healer
        case AA_H_MENDING_TOUCH:     return "Mending Touch";
        case AA_H_CRITICAL_HEALING:  return "Critical Healing";
        case AA_H_SPIRIT_CHANNEL:    return "Spirit Channel";
        case AA_H_LINGERING_RENEWAL: return "Lingering Renewal";
        case AA_H_BATTLE_MENDER:     return "Battle Mender";
        case AA_H_OVERFLOWING:       return "Overflowing";
        case AA_H_CHAIN_HEALING:     return "Chain Healing";
        // Warrior
        case AA_WAR_RAMPAGE:            return "Rampage";
        case AA_WAR_WARCRY:             return "Warcry";
        case AA_WAR_TACTICAL_MASTERY:   return "Tactical Mastery";
        case AA_WAR_LIVING_SHIELD:      return "Living Shield";
        case AA_WAR_PUNISHING_BLADE:    return "Punishing Blade";
        case AA_WAR_REND_MASTERY:       return "Rend Mastery";
        case AA_WAR_MORTAL_MASTERY:     return "Mortal Mastery";
        case AA_WAR_CLEAVING_STRIKES:   return "Cleaving Strikes";
        case AA_WAR_WHIRLWIND_MASTERY:  return "Whirlwind Mastery";
        case AA_WAR_SHIELD_MOMENTUM:    return "Shield Momentum";
        case AA_WAR_RETALIATION:        return "Retaliation";
        case AA_WAR_FURIOUS_CHARGE:     return "Furious Charge";
        case AA_WAR_IRON_WARRIOR:       return "Iron Warrior";
        case AA_WAR_BATTLE_ENDURANCE:   return "Battle Endurance";
        case AA_WAR_RAGE_ENGINE:        return "Rage Engine";
        case AA_WAR_TITANS_GRIP:        return "Titan's Grip";
        case AA_WAR_IMPROVED_DEVASTATE: return "Improved Devastate";
        case AA_WAR_UNENDING_FURY:      return "Unending Fury";
        // Paladin
        case AA_PAL_CRUSADERS_MIGHT:         return "Crusader's Might";
        case AA_PAL_JUDGE:                   return "Judge";
        case AA_PAL_EXECUTIONER:             return "Executioner";
        case AA_PAL_DIVINE_STORM_MASTERY:    return "Divine Storm Mastery";
        case AA_PAL_IMPROVED_EXORCISM:       return "Improved Exorcism";
        case AA_PAL_SERAPHIM:                return "Seraphim";
        case AA_PAL_MANDATE_OF_HEAVEN:       return "Mandate of Heaven";
        case AA_PAL_HOLY_FORTITUDE:          return "Holy Fortitude";
        case AA_PAL_RIGHTEOUS_ANGER:         return "Righteous Anger";
        case AA_PAL_IMPROVED_CONSECRATION:   return "Improved Consecration";
        case AA_PAL_IMPROVED_AVENGERS_SHIELD: return "Improved Avenger's Shield";
        case AA_PAL_FIST_OF_RECKONING:       return "Fist of Reckoning";
        case AA_PAL_BLESSING_OF_AUSTERITY:   return "Blessing of Austerity";
        case AA_PAL_SANCTUARY:               return "Sanctuary";
        case AA_PAL_IMPROVED_FLASH_OF_LIGHT: return "Improved Flash of Light";
        case AA_PAL_IMPROVED_SEAL_OF_LIGHT:  return "Improved Seal of Light";
        case AA_PAL_QUICK_BUFF:              return "Quick Buff";
        case AA_PAL_LAY_OF_HANDS_MASTERY:   return "Lay of Hands Mastery";
        case AA_PAL_HOLY_WRATH_MASTERY:      return "Holy Wrath Mastery";
        case AA_PAL_FEARLESS:                return "Fearless";
        case AA_PAL_YAULP:                   return "Yaulp";
        case AA_PAL_CELESTIAL_REGENERATION:  return "Celestial Regeneration";
        case AA_PAL_CELESTIAL_HAMMER:        return "Celestial Hammer";
        case AA_PAL_GIFT_OF_THE_KEEPER:      return "Gift of the Keeper";
        case AA_PAL_DIVINE_PROVIDENCE:       return "Divine Providence";
        case AA_PAL_PURIFYING_JUDGMENT:      return "Purifying Judgment";
        case AA_PAL_UNYIELDING_LIGHT:        return "Unyielding Light";
        // Hunter
        case AA_HUN_ARCHERY_MASTERY:         return "Archery Mastery";
        case AA_HUN_DOUBLE_BOWSHOT:          return "Double Bowshot";
        case AA_HUN_ENDLESS_QUIVER:          return "Endless Quiver";
        case AA_HUN_HEADSHOT:                return "Headshot";
        case AA_HUN_TRIPLE_ARROW:            return "Triple Arrow";
        case AA_HUN_EXPLOSIVE_ARROW:         return "Explosive Arrow";
        case AA_HUN_VOLLEY_BURST:            return "Volley Burst";
        case AA_HUN_INNATE_CAMOUFLAGE:       return "Innate Camouflage";
        case AA_HUN_VEIL_OF_MINDSHADOW:      return "Veil of Mindshadow";
        case AA_HUN_NATURES_GUIDANCE:        return "Nature's Guidance";
        case AA_HUN_ENTRAP:                  return "Entrap";
        case AA_HUN_WIND_OF_THE_SOUTH:       return "Wind of the South";
        case AA_HUN_AUSPICE:                 return "Auspice of the Hunter";
        case AA_HUN_SNARING_SHOT:            return "Snaring Shot";
        case AA_HUN_POISON_ARROW:            return "Poison Arrow";
        case AA_HUN_BURNING_ARROW:           return "Burning Arrow";
        case AA_HUN_TASTE_OF_BLOOD:          return "Taste of Blood";
        case AA_HUN_SCOUT_OF_THE_WILD:       return "Scout of the Wild";
        case AA_HUN_EAGLE_EYE:               return "Eagle Eye";
        case AA_HUN_HARMONIOUS_ARROW:        return "Harmonious Arrow";
        case AA_HUN_NATURES_MELODY:          return "Nature's Melody";
        case AA_HUN_CALL_OF_THE_WILD:        return "Call of the Wild";
        case AA_HUN_PATHFINDING:             return "Pathfinding";
        case AA_HUN_CAREFUL_AIM:             return "Careful Aim";
        case AA_HUN_QUICK_RECOVERY:          return "Quick Recovery";
        case AA_HUN_IMPROVED_SHOTS:          return "Improved Shots";
        case AA_HUN_ARCANE_QUIVER:           return "Arcane Quiver";
        case AA_HUN_CHEER_DEFENSIVE:         return "Cheer: Defensive";
        case AA_HUN_IMPROVED_BLACK_ARROW:    return "Improved Black Arrow";
        case AA_HUN_OBSIDIAN_ARROWS:         return "Obsidian Arrows";
        case AA_HUN_ENCHANTED_ARROWS:        return "Enchanted Arrows";
        case AA_HUN_VOLATILE_ENERGIES:       return "Volatile Energies";
        case AA_HUN_COMPANION_BOND:          return "Companion Bond";
        case AA_HUN_PET_ATTUNEMENT:          return "Pet Attunement";
        case AA_HUN_NATURAL_GRACE:           return "Natural Grace";
        case AA_HUN_NETHER_RAY_STING:        return "Nether Ray Sting";
        case AA_HUN_POISON_GAS:              return "Poison Gas";
        case AA_HUN_ARCANE_ANOMALY:          return "Arcane Anomaly";
        case AA_HUN_IMPROVED_BESTIAL_WRATH:  return "Improved Bestial Wrath";
        case AA_HUN_EXPLOSIVE_CHARGE:        return "Explosive Charge";
        case AA_HUN_FOCUSED_BARRAGE:         return "Focused Barrage";
        case AA_HUN_MARKED_FOR_DEATH:        return "Marked for Death";
        case AA_HUN_CHEER_OFFENSIVE:         return "Cheer: Offensive";
        case AA_HUN_CHEER_SWIFTNESS:         return "Cheer: Swiftness";
        case AA_HUN_DEDICATION:              return "Dedication";
        case AA_HUN_RANGED_MASTERY:          return "Ranged Mastery";
        case AA_HUN_IMPROVED_TRAPS:          return "Improved Traps";
        case AA_HUN_THRILL_OF_THE_HUNT:      return "Thrill of the Hunt";
        case AA_HUN_STEADY_FOCUS:            return "Steady Focus";
        case AA_HUN_PIERCING_ROUNDS:         return "Piercing Rounds";
        case AA_HUN_SURVIVAL_TACTICS:        return "Survival Tactics";
        case AA_HUN_BEAST_SYNERGY:           return "Beast Synergy";
        case AA_HUN_COORDINATED_ASSAULT:     return "Coordinated Assault";
        case AA_HUN_GO_FOR_THE_THROAT:       return "Go for the Throat";
        // Rogue
        case AA_ROG_AMBIDEXTERITY:    return "Ambidexterity";
        case AA_ROG_BACKSTAB_FOCUS:   return "Backstab Focus";
        case AA_ROG_DEATH_BLOW:       return "Death Blow";
        case AA_ROG_CHEAP_SHOT:       return "Cheap Shot";
        case AA_ROG_ESCAPE_ARTIST:    return "Escape Artist";
        case AA_ROG_FLURRY:           return "Flurry";
        case AA_ROG_HASTENED_ATTACKS: return "Hastened Attacks";
        case AA_ROG_LEG_HOLD:         return "Leg Hold";
        case AA_ROG_POISON_MASTERY:   return "Poison Mastery";
        case AA_ROG_QUICK_STRIKE:     return "Quick Strike";
        case AA_ROG_PUNCTURE:         return "Puncture";
        case AA_ROG_SHADOW_WALK:      return "Shadow Walk";
        case AA_ROG_SPEED_OF_SHADOWS: return "Speed of Shadows";
        case AA_ROG_SUBLIMATION:      return "Sublimation";
        case AA_ROG_ASSASSINS_MARK:   return "Assassin's Mark";
        case AA_ROG_LACERATE:         return "Lacerate";
        case AA_ROG_DANCING_BLADE:    return "Dancing Blade";
        case AA_ROG_FRENZY:           return "Frenzy";
        case AA_ROG_TRIP:             return "Trip";
        case AA_ROG_SLIPPERY:         return "Slippery";
        case AA_ROG_INGENUITY:        return "Ingenuity";
        case AA_ROG_TRAUMA:           return "Trauma";
        case AA_ROG_WEAK_SPOT:        return "Weak Spot";
        case AA_ROG_TRICKS:           return "Tricks";
        case AA_ROG_BLEEDING_FLURRY:  return "Bleeding Flurry";
        case AA_ROG_KILLING_SPREE:    return "Killing Spree Mastery";
        case AA_ROG_IMP_HUNGER_FOR_BLOOD: return "Improved Hunger for Blood";
        case AA_ROG_BLADE_FLURRY:     return "Blade Flurry Mastery";
        case AA_ROG_VANISH_CLONE:     return "Vanish Clone";
        case AA_ROG_IMP_RUPTURE:      return "Improved Rupture";
        case AA_ROG_DEFLECTION:       return "Deflection";
        case AA_ROG_DEBILITATION:     return "Debilitation";
        case AA_ROG_HACK_AND_SLASH:   return "Hack and Slash";
        case AA_ROG_IMP_PREMEDITATION: return "Improved Premeditation";
        case AA_ROG_IMP_RIPOSTE:      return "Improved Riposte";
        case AA_ROG_IMP_MUTILATE:     return "Improved Mutilate";
        case AA_ROG_DUPLICITY:        return "Duplicity";
        case AA_ROG_INVIGORATION:     return "Invigoration";
        case AA_ROG_POISON_MASTER:    return "Poison Master";
        case AA_ROG_SHADOWSTEP_MASTERY: return "Shadowstep Mastery";
        case AA_ROG_CHAOTIC_STAB:     return "Chaotic Stab";
        case AA_ROG_LEECHING_TOXINS:  return "Leeching Toxins";
        // General-Defensive (2113)
        case AA_G_CLEANSE_CURSE:      return "Cleanse Curse";
        // Priest
        case AA_PRI_TWINHEAL:              return "Twinheal";
        case AA_PRI_GIFT_OF_MANA:          return "Gift of Mana";
        case AA_PRI_CHANNELING_DIVINE:     return "Channeling the Divine";
        case AA_PRI_FORCEFUL_REJUVENATION: return "Forceful Rejuvenation";
        case AA_PRI_YAULP:                 return "Yaulp";
        case AA_PRI_CELESTIAL_HAMMER:      return "Celestial Hammer";
        case AA_PRI_CELESTIAL_REGEN:       return "Celestial Regeneration";
        case AA_PRI_PROLONGED_SALVE:       return "Prolonged Salve";
        case AA_PRI_QUICK_BUFF:            return "Quick Buff";
        case AA_PRI_PERSISTENT_CASTING:    return "Persistent Casting";
        case AA_PRI_LASTING_RITES:         return "Lasting Rites";
        case AA_PRI_FORCE_OF_WILL:         return "Force of Will";
        case AA_PRI_DIVINE_STUN:           return "Divine Stun";
        case AA_PRI_MARK_OF_KARNA:         return "Mark of Karna";
        case AA_PRI_INVOCATION:            return "Invocation";
        case AA_PRI_TURN_UNDEAD:           return "Turn Undead";
        case AA_PRI_RADIANT_CURE:          return "Radiant Cure";
        case AA_PRI_DIVINE_ARBITRATION:    return "Divine Arbitration";
        case AA_PRI_ARMOR_OF_WISDOM:       return "Armor of Wisdom";
        case AA_PRI_CELESTIAL_BARRIER:     return "Celestial Barrier";
        case AA_PRI_BESTOW_DIVINE_AURA:    return "Bestow Divine Aura";
        case AA_PRI_SPIRITUAL_LIGHT:       return "Spiritual Light";
        case AA_PRI_TOUCH_OF_THE_DIVINE:   return "Touch of the Divine";
        case AA_PRI_SANCTIFICATION:        return "Sanctification";
        case AA_PRI_AURA_OF_PIOUS:         return "Aura of the Pious";
        case AA_PRI_WAKE_OF_TRANQUILITY:   return "Wake of Tranquility";
        case AA_PRI_IMP_POWER_INFUSION:    return "Improved Power Infusion";
        case AA_PRI_SPREADING_MISERY:      return "Spreading Misery";
        case AA_PRI_EMPOWERED_HOLY_NOVA:   return "Empowered Holy Nova";
        case AA_PRI_CHAIN_REACTION:        return "Chain Reaction";
        case AA_PRI_HARBINGER:             return "Harbinger";
        case AA_PRI_PERSISTENCE:           return "Persistence";
        case AA_PRI_ENCROACHING_DARKNESS:  return "Encroaching Darkness";
        case AA_PRI_SHADOW_ERUPTION:       return "Shadow Eruption";
        case AA_PRI_DISCIPLE_OF_CTHUN:     return "Disciple of C'Thun";
        case AA_PRI_WANDERING_SPIRITS:     return "Wandering Spirits";
        case AA_PRI_DIVINE_GUARDIAN:       return "Divine Guardian";
        case AA_PRI_SHARED_LIFE:           return "Shared Life";
        case AA_PRI_IMP_PRAYER_OF_MENDING: return "Improved Prayer of Mending";
        case AA_PRI_INSPIRE:               return "Inspire";
        case AA_PRI_IMP_BODY_AND_SOUL:     return "Improved Body and Soul";
        case AA_PRI_IMP_LIGHTWELL:         return "Improved Lightwell";
        case AA_PRI_DIVINE_PURPOSE:        return "Divine Purpose";
        case AA_PRI_GUARDIAN_ANGEL:        return "Guardian Angel";
        case AA_PRI_IMP_SHIELD:            return "Improved Shield";
        case AA_PRI_PENANCE_MASTERY:       return "Penance Mastery";
        // Death Knight
        case AA_DK_PLAGUE_LORD:         return "Plague Lord";
        case AA_DK_PESTILENCE:          return "Pestilence";
        case AA_DK_LIFEBURN:            return "Lifeburn";
        case AA_DK_BLOOD_RITE:          return "Blood Rite";
        case AA_DK_UNHOLY_GUARD:        return "Unholy Guard";
        case AA_DK_NECROTIC_TOUCH:      return "Necrotic Touch";
        case AA_DK_IRON_SHELL:          return "Iron Shell";
        case AA_DK_FROST_ROT:           return "Frost Rot";
        case AA_DK_DEATH_PACT:          return "Death Pact";
        case AA_DK_CONTAGION_DRAIN:     return "Contagion Drain";
        case AA_DK_RUNE_AWAKENING:      return "Rune Awakening";
        case AA_DK_DEATHS_HUNGER:       return "Death's Hunger";
        case AA_DK_SCOURGE_MASTERY:     return "Scourge Mastery";
        case AA_DK_RUNE_BLADE:          return "Rune Blade Mastery";
        case AA_DK_ARCTIC_HOWL:         return "Arctic Howl";
        case AA_DK_BATTLE_FRENZY:       return "Battle Frenzy";
        case AA_DK_DEATHCHILL:          return "Deathchill Mastery";
        case AA_DK_PLAGUES_END:         return "Plague's End";
        case AA_DK_FINAL_RUNE:          return "Final Rune";
        case AA_DK_VIRULENT_PLAGUE:     return "Virulent Plague";
        case AA_DK_GHOUL_INFESTATION:   return "Ghoul Infestation";
        case AA_DK_DETONATION:          return "Detonation";
        case AA_DK_ARMY_COMMANDER:      return "Army Commander";
        case AA_DK_SOUL_ABRASION:       return "Soul Abrasion";
        case AA_DK_LEECH_TOUCH:         return "Leech Touch";
        case AA_DK_IMPROVED_HARM_TOUCH: return "Improved Harm Touch";
        // Shaman
        case AA_SHA_CANNIBALIZE:       return "Cannibalize";
        case AA_SHA_BLOOD_TITHE:       return "Blood Tithe";
        case AA_SHA_EARTHEN_PRESENCE:  return "Earthen Presence";
        case AA_SHA_TOTEMIC_MASTERY:   return "Totemic Mastery";
        case AA_SHA_ANCESTRAL_GUARD:   return "Ancestral Guard";
        case AA_SHA_WINDLORD:          return "Windlord";
        case AA_SHA_WEAPON_ATTUNEMENT: return "Weapon Attunement";
        case AA_SHA_SHOCK_RESONANCE:   return "Shock Resonance";
        case AA_SHA_SOUL_HARVEST:      return "Soul Harvest";
        case AA_SHA_ALPHA_PACK:        return "Alpha Pack";
        case AA_SHA_SPIRIT_BOND:       return "Spirit Bond";
        case AA_SHA_THUNDEROUS_STRIKE: return "Thunderous Strike";
        case AA_SHA_LAVA_SURGE:        return "Lava Surge";
        case AA_SHA_SCORCHED_EARTH:    return "Scorched Earth";
        case AA_SHA_LIGHTNING_ROD:     return "Lightning Rod";
        case AA_SHA_ELEMENTAL_OVERLOAD: return "Elemental Overload";
        case AA_SHA_GHOST_STRIKE:      return "Ghost Strike";
        case AA_SHA_SWIFT_CURRENT:     return "Swift Current";
        case AA_SHA_ANCESTRAL_BULWARK: return "Ancestral Bulwark";
        case AA_SHA_ELEMENTAL_ACCORD:  return "Elemental Accord";
        case AA_SHA_ELEMENTAL_FURY:    return "Elemental Fury";
        // Mage
        case AA_MAG_SHORT_FUSE:            return "Short Fuse";
        case AA_MAG_EXPLOSIVE_IMPACT:      return "Explosive Impact";
        case AA_MAG_SPREADING_FLAMES:      return "Spreading Flames";
        case AA_MAG_EMPOWERED_FLAMES:      return "Empowered Flames";
        case AA_MAG_METEOR_STRIKE:         return "Meteor Strike";
        case AA_MAG_DRAGONS_FIRE:          return "Dragon's Fire";
        case AA_MAG_METEOR_SHOWER:         return "Meteor Shower";
        case AA_MAG_SLOW_BURN:             return "Slow Burn";
        case AA_MAG_IMPROVED_FROSTBOLT:    return "Improved Frostbolt";
        case AA_MAG_AUGMENTED_DEEP_FREEZE: return "Augmented Deep Freeze";
        case AA_MAG_IMPROVED_DEEP_FREEZE:  return "Improved Deep Freeze";
        case AA_MAG_IMPROVED_FROST_WARD:   return "Improved Frost Ward";
        case AA_MAG_IMPROVED_ICE_BARRIER:  return "Improved Ice Barrier";
        case AA_MAG_AUGMENTED_ICY_VEINS:   return "Augmented Icy Veins";
        case AA_MAG_ARCANE_BOMBARDMENT:    return "Arcane Bombardment";
        case AA_MAG_ARCANE_SUBTLETY:       return "Arcane Subtlety";
        case AA_MAG_CHAIN_EXPLOSION:       return "Chain Explosion";
        case AA_MAG_ARCANE_ATTUNEMENT:     return "Arcane Attunement";
        case AA_MAG_FOCUSED_MAGIC:         return "Focused Magic";
        case AA_MAG_LOST_IN_TIME:          return "Lost in Time";
        case AA_MAG_MANA_BATTERY:          return "Mana Battery";
        case AA_MAG_MANA_ADEPT:            return "Mana Adept";          // renamed from Arcane Presence
        case AA_MAG_EMPOWERED_IMAGES:      return "Empowered Images";    // renamed from Flamebringer
        case AA_MAG_SCORCHED:              return "Scorched";             // renamed from Illusion of Choice
        case AA_MAG_MOLTEN_FURY:           return "Molten Fury";          // renamed from Slippery Slope
        case AA_MAG_MIRRORED_DEFENSE:      return "Mirrored Defense";
        case AA_MAG_OPTICAL_ILLUSION:      return "Optical Illusion";
        case AA_MAG_PHANTASMAL_ASSAULT:    return "Phantasmal Assault";   // renamed from Hivemind
        case AA_MAG_MIRROR_WARD:           return "Mirror Ward";          // renamed from Hallucinations
        case AA_MAG_QUICK_DAMAGE:          return "Quick Damage";
        case AA_MAG_HARVEST_OF_DRUZZIL:    return "Harvest of Druzzil";
        case AA_MAG_MANABURN:              return "Manaburn";
        case AA_MAG_SPELL_CASTING_SUBTLETY: return "Spell Casting Subtlety";
        case AA_MAG_ARCANE_NOVA:           return "Arcane Nova";          // renamed from Call of Xuzl
        case AA_MAG_IMPROVED_FAMILIAR:     return "Improved Familiar";
        case AA_MAG_FRENZIED_BURNOUT:      return "Frenzied Burnout";
        case AA_MAG_MEND_COMPANION:        return "Mend Companion";
        case AA_MAG_ELEMENTAL_BOND:        return "Elemental Bond";       // renamed from Quick Summoning
        case AA_MAG_HOST_OF_THE_ELEMENTS:  return "Host of the Elements";
        case AA_MAG_SPELL_WEAVING:         return "Spell Weaving";        // renamed from Destructive Fury
        case AA_MAG_MANA_REACTOR:          return "Mana Reactor";         // renamed from Chaotic Feedback
        case AA_MAG_PYROBLAST_OVERLOAD:    return "Pyroblast Overload";
        case AA_MAG_FIRE_BLAST_CASCADE:    return "Fire Blast Cascade";
        case AA_MAG_MOLTEN_SHELL:          return "Molten Shell";
        case AA_MAG_COMBUSTION_MASTERY:    return "Combustion Mastery";
        case AA_MAG_BLIZZARD:              return "Blizzard";
        case AA_MAG_HEATING_UP:            return "Heating Up";
        // Warlock
        case AA_WRL_THREADS_OF_DESPAIR:    return "Threads of Despair";
        case AA_WRL_MORTAL_ERADICATION:    return "Mortal Eradication";
        case AA_WRL_IMPROVED_DRAINS:       return "Improved Drains";
        case AA_WRL_IMPROVED_DRAIN_LIFE:   return "Improved Drain Life";
        case AA_WRL_IMPROVED_DRAIN_MANA:   return "Improved Drain Mana";
        case AA_WRL_IMPROVED_DRAIN_SOUL:   return "Improved Drain Soul";
        case AA_WRL_IMPROVED_CURSES:       return "Improved Curses";
        case AA_WRL_SPIRIT_LASH:           return "Spirit Lash";
        case AA_WRL_UMBRAL_LEECH:          return "Umbral Leech";
        case AA_WRL_BURNING_SOUL:          return "Burning Soul";
        case AA_WRL_MOLTEN_SKIN:           return "Molten Skin";
        case AA_WRL_BACKDRAFT:             return "Backdraft";
        case AA_WRL_CRITICAL_MASS:         return "Critical Mass";
        case AA_WRL_EMBERSTORM:            return "Emberstorm";
        case AA_WRL_TENEBROUS_REACH:       return "Tenebrous Reach";
        case AA_WRL_DESTRUCTIVE_PATH:      return "Destructive Path";
        case AA_WRL_NETHER_PORTAL:         return "Nether Portal";
        case AA_WRL_INFERNAL_VOLCANO:      return "Infernal Volcano";
        case AA_WRL_DEMONIC_SYNERGY:       return "Demonic Synergy";
        case AA_WRL_EMPOWERED_DEMONS:      return "Empowered Demons";
        case AA_WRL_DEMONIC_KNOWLEDGE:     return "Demonic Knowledge";
        case AA_WRL_WELL_OF_SOULS:         return "Well of Souls";
        case AA_WRL_SOUL_BARRAGE:          return "Soul Barrage";
        case AA_WRL_SOULSTORM:             return "Soulstorm";
        case AA_WRL_CIRCLE_OF_THE_DAMNED:  return "Circle of the Damned";
        case AA_WRL_SOUL_MIRROR:           return "Soul Mirror";
        case AA_WRL_WAKE_THE_DEAD:         return "Wake the Dead";
        case AA_WRL_FEARSTORM:             return "Fearstorm";
        case AA_WRL_LIFEBURN:              return "Lifeburn";
        case AA_WRL_DIRE_CHARM:            return "Dire Charm";
        case AA_WRL_SOUL_ABRASION:         return "Soul Abrasion";
        case AA_WRL_LEECH_TOUCH:           return "Leech Touch";
        case AA_WRL_IMPROVED_HARM_TOUCH:   return "Improved Harm Touch";
        case AA_WRL_SUSPENDED_MINION:      return "Suspended Minion";
        case AA_WRL_FEIGNED_MINION:        return "Feigned Minion";
        case AA_WRL_SPELL_CASTING_SUBTLETY: return "Spell Casting Subtlety";
        // Warrior (new)
        case AA_WAR_VENGEFUL_BULWARK:      return "Vengeful Bulwark";
        // DK (new)
        case AA_DK_CORRUPTED_CARAPACE:     return "Corrupted Carapace";
        // Druid (BUILT 2026-06-26)
        case AA_DRU_IMP_LACERATE_RAKE:     return "Improved Lacerate & Rake";
        case AA_DRU_RIP_AND_TEAR:          return "Rip and Tear";
        case AA_DRU_BEAST_WITHIN:          return "Beast Within";
        case AA_DRU_IMPROVED_BEAST_FORM:   return "Improved Beast Form";
        case AA_DRU_AUGMENTED_BEAST_FORM:  return "Augmented Beast Form";
        case AA_DRU_IMPROVED_BERSERK:      return "Improved Berserk";
        case AA_DRU_IMPROVED_FAERIE_FIRE:  return "Improved Faerie Fire";
        case AA_DRU_WRATH_OF_THE_WILD:     return "Wrath of the Wild";
        case AA_DRU_NATURES_TENACITY:      return "Nature's Tenacity";
        case AA_DRU_SAVAGE_SWIPE:          return "Savage Swipe";
        case AA_DRU_CELESTIAL_IMPACT:      return "Celestial Impact";
        case AA_DRU_CELESTIAL_WRATH:       return "Celestial Wrath";
        case AA_DRU_ANCESTRAL_SPIRITS:     return "Ancestral Spirits";
        case AA_DRU_IMPROVED_TYPHOON:      return "Improved Typhoon";
        case AA_DRU_SUNFIRE:               return "Sunfire";
        case AA_DRU_ECLIPSE_MASTERY:       return "Eclipse Mastery";
        case AA_DRU_NATURES_CHOSEN:        return "Nature's Chosen";
        case AA_DRU_SPIRIT_OF_THE_WOOD:    return "Spirit of the Wood";
        case AA_DRU_HEALING_ADEPT:         return "Healing Adept";
        case AA_DRU_HEALING_GIFT:          return "Healing Gift";
        case AA_DRU_NATURES_REMEDY:        return "Nature's Remedy";
        case AA_DRU_LIVING_SEED:           return "Living Seed";
        case AA_DRU_PACK_CHLOROPLAST:      return "Pack Chloroplast";
        case AA_DRU_SWIFTMEND_MASTERY:     return "Swiftmend Mastery";
        case AA_DRU_RADIANT_CURE:          return "Radiant Cure";
        case AA_DRU_CALL_OF_THE_WILD:      return "Call of the Wild";
        case AA_DRU_INNATE_CAMOUFLAGE:     return "Innate Camouflage";
        case AA_DRU_STAMPEDING_ROAR:       return "Stampeding Roar";
        case AA_DRU_IMPROVED_THORNS:       return "Improved Thorns";
        case AA_DRU_AUGMENTED_THORNS:      return "Augmented Thorns";
        case AA_DRU_SURVIVAL_INSTINCTS:    return "Survival Instincts";
        case AA_DRU_IRONFUR:               return "Ironfur";
        case AA_DRU_HEART_OF_THE_WILD:     return "Heart of the Wild";
        case AA_DRU_FERAL_CHARGE_MASTERY:  return "Feral Charge Mastery";
        case AA_DRU_CHAOTIC_STAB:          return "Chaotic Stab (Cat)";
        // Legacy
        case AA_TEMPER:             return "Temper";
        default:                    return "Unknown AA";
    }
}

static const char* GetAADesc(uint32 aaId)
{
    switch (aaId)
    {
        case AA_G_IRON_WILL:              return "+200 HP per rank";
        case AA_G_SANCTUM_ESSENCE:        return "+20 to all primary stats per rank";
        case AA_T_TITANS_BLOOD:           return "+200 HP per rank (Tank archetype)";
        case AA_TEMPER:                   return "Sacrifice gear for Gear XP (free)";
        case AA_HUN_STEADY_FOCUS:         return "+60/+120/+180 ranged attack power";
        case AA_HUN_PIERCING_ROUNDS:      return "+5%/+10%/+15% armor penetration (ranged)";
        case AA_HUN_SURVIVAL_TACTICS:     return "+2%/+4%/+7% dodge chance";
        case AA_HUN_BEAST_SYNERGY:        return "+3%/+6%/+10% damage while pet or guardian is alive";
        case AA_HUN_COORDINATED_ASSAULT:  return "+5%/+10%/+15% damage to the target your pet is attacking";
        case AA_HUN_GO_FOR_THE_THROAT:    return "Auto-attacks 10%/20%/30% chance: pet bites for 40/60/80% of the hit";
        case AA_P_BLOODSCENT:             return "+10%/+20%/+30% pet damage to targets below 35% HP";
        case AA_P_MENDING_BOND:           return "Pet/guardian regens 2%/4%/6% max HP per second while below 50% HP";
        // Priest AAs
        case AA_PRI_TWINHEAL:             return "Heal spells 5%/10%/15% chance to fire twice";
        case AA_PRI_GIFT_OF_MANA:         return "Heals 5%/10%/15% chance to make next spell cost 0 mana";
        case AA_PRI_CHANNELING_DIVINE:    return "Next 5/8/12 heals fire twice. 3min CD";
        case AA_PRI_FORCEFUL_REJUVENATION:return "ONE-SHOT: Instantly reset all spell cooldowns. 10min CD";
        case AA_PRI_YAULP:                return "Melee speed +15/25/40% and dmg +10/20/30% for 30s. 2min CD";
        case AA_PRI_CELESTIAL_HAMMER:     return "Conjure hammer: 3 holy strikes, each 80/110/150%% SP. 2min CD";
        case AA_PRI_CELESTIAL_REGEN:      return "Free HoT: 5/8/12%% max HP per tick over 30s. 5min CD";
        case AA_PRI_MARK_OF_KARNA:        return "Holy/shadow spell marks target: +8/15/25%% dmg from you for 15s";
        case AA_PRI_TURN_UNDEAD:          return "Holy spells vs undead below 35%% HP: 5/10/20%% instant kill chance";
        case AA_PRI_RADIANT_CURE:         return "Dispel also removes disease+poison; R2: +curse; R3: AoE";
        case AA_PRI_DIVINE_ARBITRATION:   return "Equalize HP%% between you and active pets/guardians. 3/2/1.5min CD";
        case AA_PRI_CELESTIAL_BARRIER:    return "Absorb shield = 30/50/75%% SP for 10s. 60s CD";
        case AA_PRI_BESTOW_DIVINE_AURA:   return "Target invulnerable for 3/5/8s. 5min CD";
        case AA_PRI_TOUCH_OF_THE_DIVINE:  return "Attackers take 15/25/40%% SP as holy on each melee hit";
        case AA_PRI_IMP_POWER_INFUSION:   return "+5/8/12%% all magic damage. (PI CD-reduction half stubbed)";
        case AA_PRI_SPREADING_MISERY:     return "SW:P/VT/DP jumps to nearest enemy on kill; R2: +10%% shadow dmg";
        case AA_PRI_CHAIN_REACTION:       return "Mind Blast 10/20/30%% chance: 75%% to random nearby enemy";
        case AA_PRI_HARBINGER:            return "SW:Death hits all in 6/8/10 yd; +10/20/30%% dmg";
        case AA_PRI_ENCROACHING_DARKNESS: return "SW:P/VT/DP +5/10/15%% dmg per tick";
        case AA_PRI_INSPIRE:              return "After empowered shadow spell: all active pets +8/15/25%% dmg for 10s";
        // Priest deferred/stubbed
        case AA_PRI_PROLONGED_SALVE:      return "Phase 2+ — HoT duration extension (hook not available)";
        case AA_PRI_QUICK_BUFF:           return "Next 3/5/7 spells at half cast time — approximated as haste burst. 90s CD";
        case AA_PRI_PERSISTENT_CASTING:   return "Phase 2+ — cast-through-interrupt (hook not available)";
        case AA_PRI_LASTING_RITES:        return "Phase 2+ — long self-buff extension (aura-apply hook not available)";
        case AA_PRI_PERSISTENCE:          return "Phase 2+ — DoT-expiry burst (DoT-expiry hook not available)";
        case AA_PRI_SHADOW_ERUPTION:      return "Phase 2+ — requires Persistence (not yet built)";
        default:                          return "Phase 2+ mechanic — tracked, not yet active";
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
class mod_aa_system_commandscript : public CommandScript
{
public:
    mod_aa_system_commandscript() : CommandScript("mod_aa_system_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable aaCommandTable =
        {
            { "sync",      HandleAaSyncCommand,        SEC_PLAYER,     Console::No },
            { "list",      HandleAaListCommand,        SEC_PLAYER,     Console::No },
            { "buy",       HandleAaBuyCommand,          SEC_PLAYER,     Console::No },
            { "bleed",     HandleAaBleedCommand,        SEC_PLAYER,     Console::No },
            { "convert",   HandleAaConvertCommand,      SEC_PLAYER,     Console::No },
            { "use",       HandleAaUseCommand,          SEC_PLAYER,     Console::No },
            { "respec",    HandleAaRespecCommand,       SEC_PLAYER,     Console::No },
            { "grant",     HandleAaGrantCommand,        SEC_GAMEMASTER, Console::No },
            { "testall",   HandleAaTestAllCommand,      SEC_GAMEMASTER, Console::Yes },
            { "addpoints", HandleAaAddPointsCommand,    SEC_GAMEMASTER, Console::No },
            { "add",      HandleAaAddPointsCommand,    SEC_GAMEMASTER, Console::No },
            { "",          HandleAaInfoCommand,         SEC_PLAYER,     Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "aa", aaCommandTable },
        };
        return commandTable;
    }

    static bool HandleAaInfoCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        AaData const& data = GetAaData(player->GetGUID().GetCounter());

        handler->PSendSysMessage("|cff00ccff[AA System]|r");
        handler->PSendSysMessage("  Points earned: {}", data.pointsEarned);
        handler->PSendSysMessage("  Points spent:  {}", data.pointsSpent);
        handler->PSendSysMessage("  Points avail:  |cff00ff00{}|r", data.PointsAvailable());
        uint32 threshold = ComputeAAThreshold(player);
        handler->PSendSysMessage("  AA XP progress: {} / {} ({}%)",
            (uint32)data.aaXp, threshold,
            threshold > 0 ? (uint32)data.aaXp * 100u / threshold : 0u);
        handler->PSendSysMessage("  AAs purchased: {}", data.purchased.size());
        handler->PSendSysMessage("Type |cffffd700.aa list|r to see purchased AAs.");
        handler->PSendSysMessage("Type |cffffd700.aa buy <id>|r to purchase.");
        return true;
    }

    static bool HandleAaSyncCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        AaData const& data = GetAaData(player->GetGUID().GetCounter());
        handler->PSendSysMessage("|cff00ccff[AA]|r Syncing {} purchased AA(s) to addon.", data.purchased.size());
        for (auto const& [aaId, rank] : data.purchased)
            handler->PSendSysMessage("  → ID {} |cffffd700{}|r rank {}", aaId, GetAAName(aaId), (uint32)rank);
        PushAADataToClient(player);
        return true;
    }

    static bool HandleAaListCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        AaData const& data = GetAaData(player->GetGUID().GetCounter());

        if (data.purchased.empty())
        {
            handler->SendSysMessage("No AAs purchased yet.");
            return true;
        }

        handler->SendSysMessage("|cff00ccff[Purchased AAs]|r");
        for (auto const& [aaId, rank] : data.purchased)
        {
            handler->PSendSysMessage("  ID {}  |cffffd700{}|r  Rank {}/{}",
                aaId, GetAAName(aaId), (uint32)rank, (uint32)AA_MAX_RANK);
        }
        return true;
    }

    static bool HandleAaBuyCommand(ChatHandler* handler, std::string_view args)
    {
        uint32 aaId = 0;
        for (char c : args)
        {
            if (c == ' ') continue;
            if (c < '0' || c > '9')
            {
                handler->SendSysMessage("|cffff0000[AA]|r Usage: .aa buy <id>");
                return true;
            }
            aaId = aaId * 10 + (c - '0');
        }
        if (aaId == 0)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Usage: .aa buy <id>");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        uint32 guid    = player->GetGUID().GetCounter();
        AaData& data   = GetAaData(guid);

        uint8 reqLevel = GetAATierLevel(aaId);
        if (player->GetLevel() < reqLevel)
        {
            handler->PSendSysMessage("|cffff0000[AA]|r {} requires level {}.",
                GetAAName(aaId), (uint32)reqLevel);
            return true;
        }

        uint8 currentRank = 0;
        auto it = data.purchased.find(aaId);
        if (it != data.purchased.end())
            currentRank = it->second;

        uint8 aaMaxRank = GetAAMaxRank(aaId);
        if (currentRank >= aaMaxRank)
        {
            handler->PSendSysMessage("|cffff0000[AA]|r {} is already at max rank ({}).",
                GetAAName(aaId), (uint32)aaMaxRank);
            return true;
        }

        uint8 nextRank = currentRank + 1;
        uint8 cost = GetAARankCost(aaId, nextRank);

        if (data.PointsAvailable() < cost)
        {
            handler->PSendSysMessage(
                "|cffff0000[AA]|r Not enough points. Need {}, have {}.",
                (uint32)cost, data.PointsAvailable());
            return true;
        }
        data.pointsSpent += cost;
        data.purchased[aaId] = nextRank;

        ApplyAAStat(player, aaId, 1, true);
        SaveAAPoints(player);
        SavePurchasedAA(player, aaId, nextRank);

        handler->PSendSysMessage(
            "|cff00ff00[AA]|r Purchased |cffffd700{}|r rank {}/{}. ({} points remaining)",
            GetAAName(aaId), (uint32)nextRank, (uint32)aaMaxRank,
            data.PointsAvailable());

        SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:BOUGHT:{}:{}:{}:{}", aaId, (uint32)nextRank,
            data.pointsEarned, data.pointsSpent));
        return true;
    }

    static bool HandleAaBleedCommand(ChatHandler* handler, std::string_view args)
    {
        uint32 pct = 0;
        bool   hasDigit = false;
        for (char c : args)
        {
            if (c == ' ') continue;
            if (c < '0' || c > '9')
            {
                handler->SendSysMessage("|cffff0000[AA]|r Usage: .aa bleed <0|10|20|...|100>");
                return true;
            }
            pct = pct * 10 + (c - '0');
            hasDigit = true;
        }
        if (!hasDigit || pct > 100 || pct % 10 != 0)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Bleed must be 0, 10, 20, ..., or 100.");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        AaData& data   = GetAaData(player->GetGUID().GetCounter());
        data.bleedPct  = (uint8)pct;
        SaveAAPoints(player);

        handler->PSendSysMessage(
            "|cff00ff00[AA]|r XP bleed set to |cffffd700{}%|r. {}% to leveling, {}% to AA.",
            pct, 100u - pct, pct);

        SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:BLEED:{}", pct));
        return true;
    }

    static bool HandleAaConvertCommand(ChatHandler* handler, std::string_view args)
    {
        uint32 points = 0;
        bool hasDigit = false;
        for (char c : args)
        {
            if (c == ' ') continue;
            if (c < '0' || c > '9')
            {
                handler->SendSysMessage("|cffff0000[AA]|r Usage: .aa convert <points>  (minimum 5)");
                return true;
            }
            points = points * 10 + (c - '0');
            hasDigit = true;
        }
        if (!hasDigit || points == 0)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Usage: .aa convert <points>  (minimum 5)");
            return true;
        }
        if (points < 5)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Minimum conversion is 5 points.");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        AaData& data   = GetAaData(player->GetGUID().GetCounter());

        if (data.PointsAvailable() < points)
        {
            handler->PSendSysMessage(
                "|cffff0000[AA]|r Not enough points. Need {}, have {}.",
                points, data.PointsAvailable());
            return true;
        }

        uint32 gxpGain = points * 100;
        data.pointsSpent += points;
        SaveAAPoints(player);

        GearTiers_AddGXP(player, gxpGain);

        handler->PSendSysMessage(
            "|cff00ff00[AA]|r Converted |cffffd700{}|r point{} → |cff1eff00{} GXP|r. ({} points remaining)",
            points, (points != 1 ? "s" : ""), gxpGain, data.PointsAvailable());

        SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:CONVERTED:{}:{}:{}",
            gxpGain, data.pointsEarned, data.pointsSpent));
        return true;
    }

    static bool HandleAaUseCommand(ChatHandler* handler, std::string_view args)
    {
        uint32 aaId = 0;
        bool hasDigit = false;
        for (char c : args)
        {
            if (c == ' ') continue;
            if (c < '0' || c > '9')
            {
                handler->SendSysMessage("|cffff0000[AA]|r Usage: .aa use <id>");
                return true;
            }
            aaId = aaId * 10 + (c - '0');
            hasDigit = true;
        }
        if (!hasDigit || aaId == 0)
        {
            handler->SendSysMessage("|cffff0000[AA]|r Usage: .aa use <id>");
            return true;
        }
        return SanctumAA_HandleActivate(handler->GetSession()->GetPlayer(), aaId, handler);
    }

    static bool HandleAaRespecCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        AaData& data   = GetAaData(player->GetGUID().GetCounter());

        if (data.purchased.empty())
        {
            handler->SendSysMessage("No AAs to respec.");
            return true;
        }

        if (!player->HasEnoughMoney(AA_RESPEC_COST))
        {
            handler->PSendSysMessage("|cffff0000[AA]|r Respec costs 50g. You don't have enough gold.");
            return true;
        }

        player->ModifyMoney(-(int32)AA_RESPEC_COST);
        RemoveAllAAStats(player);

        uint32 refunded = data.pointsSpent;
        data.pointsSpent = 0;
        data.purchased.clear();

        DeleteAllPurchasedAA(player);
        SaveAAPoints(player);

        handler->PSendSysMessage(
            "|cff00ff00[AA]|r Respec complete. {} points refunded. {} available.",
            refunded, data.PointsAvailable());

        SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:RESPEC:{}", data.pointsEarned));
        return true;
    }

    static bool HandleAaGrantCommand(ChatHandler* handler, std::string_view args)
    {
        std::string argStr(args);
        std::istringstream ss(argStr);
        uint32 aaId = 0;
        uint32 rankIn = 0;
        ss >> aaId >> rankIn;

        if (aaId == 0 || rankIn < 1 || rankIn > AA_MAX_RANK)
        {
            handler->PSendSysMessage("Usage: .aa grant <id> <rank 1-{}>", (uint32)AA_MAX_RANK);
            return true;
        }

        Player* player = handler->getSelectedPlayerOrSelf();
        uint8  rank  = (uint8)rankIn;
        uint32 guid  = player->GetGUID().GetCounter();
        AaData& data = GetAaData(guid);

        auto it = data.purchased.find(aaId);
        if (it != data.purchased.end())
            ApplyAAStat(player, aaId, it->second, false);

        data.purchased[aaId] = rank;
        ApplyAAStat(player, aaId, rank, true);
        SavePurchasedAA(player, aaId, rank);

        handler->PSendSysMessage("Granted |cffffd700{}|r rank {} to {}.",
            GetAAName(aaId), (uint32)rank, player->GetName());
        return true;
    }

    // ---------------------------------------------------------------------
    // .aa testall  — GM: grant EVERY recognized AA at max rank to the target.
    // Testing aid: instantly outfits a character with the entire AA catalog so
    // its passives/procs/actives are all live. NOTE: an AA still only DOES
    // something if the character can actually perform the trigger (cast the
    // spell / have the pet / hold the resource) — so run this on multiclass
    // test chars that cover the classes you want to exercise.
    // ---------------------------------------------------------------------
    static bool HandleAaTestAllCommand(ChatHandler* handler, std::string_view args)
    {
        // Optional <name>: lets a console/SOAP session target a player by name
        // (those sessions have no "selected" player). In-game GMs can omit it
        // and it falls back to the selected target / self.
        Player* player = nullptr;
        std::string name(args);
        while (!name.empty() && (name.front() == ' ')) name.erase(name.begin());
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r' || name.back() == '\n'))
            name.pop_back();
        if (!name.empty())
            player = ObjectAccessor::FindPlayerByName(name, true);
        if (!player)
            player = handler->getSelectedPlayerOrSelf();
        if (!player)
        {
            handler->SendSysMessage("|cffff0000[AA]|r No valid target. Usage: .aa testall [character name]");
            return true;
        }

        uint32 guid = player->GetGUID().GetCounter();
        AaData& data = GetAaData(guid);

        // Clear current AA state cleanly, then re-grant everything at max rank.
        RemoveAllAAStats(player);
        DeleteAllPurchasedAA(player);
        data.purchased.clear();

        // ID ranges spanning every tree: General(2xxx), Pet(3xxx),
        // Archetype(4xxx), Class(5xxx), misc/Temper(9xxx). GetAAName returns
        // "Unknown AA" for gaps/scrapped IDs, which we skip — so this stays
        // self-maintaining as the catalog changes.
        static const std::pair<uint32, uint32> s_ranges[] = {
            { 2000u, 2299u }, { 3000u, 3299u }, { 4000u, 4399u },
            { 5000u, 5999u }, { 9000u, 9099u }
        };

        uint32 granted = 0;
        for (auto const& range : s_ranges)
        {
            for (uint32 id = range.first; id <= range.second; ++id)
            {
                if (std::string(GetAAName(id)) == "Unknown AA")
                    continue;
                uint8 maxR = GetAAMaxRank(id);
                data.purchased[id] = maxR;
                ApplyAAStat(player, id, maxR, true);
                SavePurchasedAA(player, id, maxR);
                ++granted;
            }
        }

        SaveAAPoints(player);
        PushAADataToClient(player);

        handler->PSendSysMessage(
            "|cff00ff00[AA]|r TESTALL: granted |cffffd700{}|r AAs at max rank to {}.",
            granted, player->GetName());
        handler->PSendSysMessage(
            "|cffffff00Note:|r each AA only fires for spells/pets this character's classes can use.");
        return true;
    }

    static bool HandleAaAddPointsCommand(ChatHandler* handler, std::string_view args)
    {
        uint32 amount = 0;
        for (char c : args)
        {
            if (c == ' ') continue;
            if (c < '0' || c > '9')
            {
                handler->SendSysMessage("Usage: .aa addpoints <amount>");
                return true;
            }
            amount = amount * 10 + (c - '0');
        }
        if (amount == 0)
        {
            handler->SendSysMessage("Usage: .aa addpoints <amount>");
            return true;
        }

        Player* player = handler->getSelectedPlayerOrSelf();
        uint32 guid  = player->GetGUID().GetCounter();
        AaData& data = GetAaData(guid);

        data.pointsEarned += amount;
        SaveAAPoints(player);

        uint32 threshold = ComputeAAThreshold(player);
        SendToAAAddon(player, Acore::StringFormat("SANCTUMAA:INIT:{}:{}:{}:{}:{}",
            data.pointsEarned, data.pointsSpent,
            (uint32)data.aaXp, (uint32)data.bleedPct, threshold));

        handler->PSendSysMessage(
            "|cff00ff00[AA]|r Added {} points to {}. Available: {}",
            amount, player->GetName(), data.PointsAvailable());
        return true;
    }
};

// ---------------------------------------------------------------------------
// OOC speed helpers — Sprinter (2201) + Pathfinding (5223) combined.
// Called by PlayerScript enter/leave combat + login.
// ---------------------------------------------------------------------------
static void ApplyOOCSpeed(Player* player)
{
    uint8 sprRank  = SanctumAA::GetRank(player, AA_G_SPRINTER);
    uint8 pathRank = SanctumAA::GetRank(player, AA_HUN_PATHFINDING);
    if (sprRank == 0 && pathRank == 0)
        return;
    static const float sprSpeed[]  = { 0.0f, 0.05f, 0.10f, 0.15f };
    static const float pathSpeed[] = { 0.0f, 0.05f, 0.08f, 0.12f };
    float bonus = sprSpeed[std::min<uint8>(sprRank, 3)]
                + pathSpeed[std::min<uint8>(pathRank, 3)];
    player->SetSpeedRate(MOVE_RUN, 1.0f + bonus);
}

static void RemoveOOCSpeed(Player* player)
{
    if (SanctumAA::GetRank(player, AA_G_SPRINTER) == 0 &&
        SanctumAA::GetRank(player, AA_HUN_PATHFINDING) == 0)
        return;
    player->SetSpeedRate(MOVE_RUN, 1.0f);
}

// ---------------------------------------------------------------------------
// Player hooks
// ---------------------------------------------------------------------------
class mod_aa_system_playerscript : public PlayerScript
{
public:
    mod_aa_system_playerscript() : PlayerScript("mod_aa_system_playerscript") {}

    void OnPlayerLogin(Player* player) override
    {
        LoadAAData(player);
        ApplyAllAAStats(player);
        PushAADataToClient(player);

        // Active AAs are activated via the SanctumAA "Sanctum Abilities" Lua bar
        // (client-side, sends ".aa use <id>"). No server-side spell/skill grant needed.

        // Sprinter / Pathfinding — apply OOC speed bonus on login
        if (!player->IsInCombat())
            ApplyOOCSpeed(player);

        AaData const& data = GetAaData(player->GetGUID().GetCounter());
        if (data.PointsAvailable() > 0)
        {
            std::string msg = "|cff00ccff[AA]|r You have |cff00ff00" +
                std::to_string(data.PointsAvailable()) +
                "|r AA point" +
                (data.PointsAvailable() != 1 ? "s" : "") +
                " available. Type |cffffd700.aa|r to manage them.";
            ChatHandler(player->GetSession()).SendSysMessage(msg.c_str());
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        uint32 guid = player->GetGUID().GetCounter();
        SanctumAA_ClearActivateState(guid);
        RemoveAaData(guid);
    }

    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        RemoveOOCSpeed(player);
    }

    void OnPlayerLeaveCombat(Player* player) override
    {
        ApplyOOCSpeed(player);
    }

    void OnPlayerJustDied(Player* player) override
    {
        SanctumAA_ClearActivateState(player->GetGUID().GetCounter());

        // Tempered Body — restore a fraction of the death durability loss.
        // AC applies floor(maxDurability * 0.10) loss per item at death.
        // We restore rankPct of that loss immediately after.
        uint8 rank = SanctumAA::GetRank(player, AA_G_TEMPERED_BODY);
        if (rank == 0)
            return;

        static const float restorePct[] = { 0.0f, 0.10f, 0.20f, 0.30f };
        float restoreFrac = restorePct[std::min<uint8>(rank, 3)];

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (maxDur == 0)
                continue;

            uint32 lostDur    = (uint32)(maxDur * 0.10f);
            uint32 restoreDur = (uint32)(lostDur * restoreFrac);
            if (restoreDur == 0)
                continue;

            uint32 curDur = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            uint32 newDur = std::min(curDur + restoreDur, maxDur);
            item->SetUInt32Value(ITEM_FIELD_DURABILITY, newDur);
            item->SetState(ITEM_CHANGED, player);
        }
    }

    void OnPlayerCreate(Player* player) override
    {
        uint32 guid = player->GetGUID().GetCounter();

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO character_aa_points (guid, aa_xp, points_earned, points_spent, aa_bleed_pct) "
            "VALUES ({}, 0, 0, 0, 0)", guid);

        // Grant Temper R1 for free
        CharacterDatabase.Execute(
            "INSERT IGNORE INTO character_aa_purchased (guid, aa_id, aa_rank) VALUES ({}, {}, 1)",
            guid, (uint32)AA_TEMPER);

        ChatHandler(player->GetSession()).SendSysMessage(
            "|cff00ccff[AA]|r |cffffd700Temper|r (Rank 1) granted — sacrifice gear to earn Gear XP. "
            "Type |cffffd700.aa|r to manage Alternate Advancement.");
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (amount == 0)
            return;

        AaData& data = GetAaData(player->GetGUID().GetCounter());
        if (data.bleedPct == 0)
            return;

        uint64 aaXp = (uint64)amount * data.bleedPct / 100u;
        amount      = (uint32)((uint64)amount * (100u - data.bleedPct) / 100u);

        if (aaXp > 0)
            AwardAAXP(player, aaXp);
    }
};

// ---------------------------------------------------------------------------
// Chaotic Stab (AA 5341) — strip Backstab positional + weapon requirements
//
// Backstab in 3.3.5a is gated by:
//   1. SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET — custom attribute set by AC core at
//      startup via SpellInfo::_LoadSpellCustomAttr; the Spell.cpp CheckCast checks this
//      and returns SPELL_FAILED_NOT_BEHIND if target is in front.
//   2. EquippedItemClass = 2 (weapon) with EquippedItemSubClassMask = 0x4 (dagger) — the
//      core CheckCast returns SPELL_FAILED_EQUIPPED_ITEM_CLASS if no dagger is equipped.
//
// Since Sanctum is solo, a global strip at startup is the cleanest solution.
// Players without Chaotic Stab are unaffected mechanically (the damage reduction for
// front attacks is enforced in aa_class.cpp ModifySpellDamageTaken, not here).
// ---------------------------------------------------------------------------
class mod_aa_chaotic_stab_worldscript : public WorldScript
{
public:
    mod_aa_chaotic_stab_worldscript() : WorldScript("mod_aa_chaotic_stab_worldscript") {}

    // OnStartup fires after sSpellMgr is fully loaded (same timing mod-multiclass
    // uses for its spell strips). OnAfterConfigLoad runs too early — the spell
    // store isn't populated yet, so GetSpellInfo() would return null for all IDs.
    void OnStartup() override
    {
        // All WotLK Backstab spell IDs (all ranks)
        static const uint32 backstabIds[] = { 53, 2589, 2590, 2591, 8721, 11279, 11280, 11281, 25300, 26863, 48656, 48657 };

        uint32 stripped = 0;
        for (uint32 spellId : backstabIds)
        {
            SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId);
            if (!si)
                continue;

            // const_cast is the same pattern used by AC's StripDruidForms and other
            // startup-time spell info modifications in 3.3.5a modules.
            SpellInfo* info = const_cast<SpellInfo*>(si);

            // 1. Strip behind-target requirement
            info->AttributesCu &= ~uint32(SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET);

            // 2. Strip dagger-only weapon class requirement
            if (info->EquippedItemClass == 2 /* ITEM_CLASS_WEAPON */)
            {
                info->EquippedItemClass          = -1;
                info->EquippedItemSubClassMask   = 0;
                info->EquippedItemInventoryTypeMask = 0;
            }
            ++stripped;
        }
        if (stripped > 0)
            LOG_INFO("server.loading", "[mod-aa-system] Chaotic Stab (Rogue): stripped positional + weapon requirements from {} Backstab spells.", stripped);

        // ── Druid Chaotic Stab (5934) — strip Shred and Ravage positional + weapon requirements ──
        // Shred in 3.3.5a: SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET + weapon class requirement
        // Shred IDs (all ranks): 5221, 6800, 8992, 9829, 9830, 27001 (VERIFY), 48571, 48572
        // Ravage IDs: 6785, 6787 (only 2 ranks as of WotLK, used from stealth/pounce)
        // FLAG: Shred 27001 may conflict with a Druid Swipe ID — verify before deploy.
        {
            static const uint32 shredIds[]  = { 5221, 6800, 8992, 9829, 9830, 48571, 48572 };
            static const uint32 ravageIds[] = { 6785, 6787 };

            uint32 druStripped = 0;
            auto stripPositional = [&](const uint32* ids, uint32 count)
            {
                for (uint32 i = 0; i < count; ++i)
                {
                    SpellInfo const* si = sSpellMgr->GetSpellInfo(ids[i]);
                    if (!si) continue;
                    SpellInfo* info = const_cast<SpellInfo*>(si);
                    info->AttributesCu &= ~uint32(SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET);
                    if (info->EquippedItemClass == 2)
                    {
                        info->EquippedItemClass             = -1;
                        info->EquippedItemSubClassMask      = 0;
                        info->EquippedItemInventoryTypeMask = 0;
                    }
                    ++druStripped;
                }
            };
            stripPositional(shredIds,  7);
            stripPositional(ravageIds, 2);
            LOG_INFO("server.loading", "[mod-aa-system] Chaotic Stab (Druid 5934): stripped positional requirements from {} Shred/Ravage spells.", druStripped);
        }

        // ── Augmented Deep Freeze (5709) — strip "target must be stun-immune" and
        //    "target must be chilled/frozen" requirements from Deep Freeze (44572).
        //    Deep Freeze in 3.3.5a uses SpellEffectImplicitTargetConditions that we
        //    cannot remove without core changes; the secondary approach is to strip
        //    SPELL_ATTR0_ONLY_STEALTHED flag (used as a proxy in some implementations)
        //    and the TargetCreatureType stun-immune filter.
        //    PARTIAL: we zero out any EquippedItem restrictions and clear
        //    SPELL_ATTR2_NO_TARGET_PER_SECOND_COST which some forks use for the
        //    "stun-immune only" gate. The real condition is in the spell effect data
        //    (SPELL_EFFECT_STUN_AND_DAMAGE requires a specific target condition in
        //    SpellImplicitTargetEntry) — not removable via SpellInfo flags alone.
        //    In-game: players with AA 5709 will get the damage bonus (ModifySpellDamageTaken)
        //    but the built-in "stun-immune" restriction from client-side targeting may remain.
        //    Flag as PARTIAL — works for the damage boost; targeting gate may persist.
        {
            SpellInfo const* dfSi = sSpellMgr->GetSpellInfo(44572);
            if (dfSi)
            {
                SpellInfo* dfInfo = const_cast<SpellInfo*>(dfSi);
                // Strip Attributes that enforce "target must be immune to stun"
                // In vanilla/wotlk 3.3.5a the flag SPELL_ATTR3_REQUIRES_COMBO_POINTS (0x80)
                // is NOT the culprit. Clear SPELL_ATTR1_CHANNELED_1 workaround not needed.
                // The real guard is in the lua condition; best we can do is note this is partial.
                // No flag to strip that would help without core changes — leave as PARTIAL.
                (void)dfInfo;
            }
            LOG_INFO("server.loading", "[mod-aa-system] Augmented Deep Freeze (5709): damage bonus live; target restriction strip is PARTIAL (SpellInfo condition not accessible via flag edit).");
        }
    }
};

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------
void AddSC_mod_aa_system()
{
    LOG_INFO("server.loading", "[mod-aa-system] Module loaded.");
    new mod_aa_system_commandscript();
    new mod_aa_system_playerscript();
    new mod_aa_chaotic_stab_worldscript();
}
