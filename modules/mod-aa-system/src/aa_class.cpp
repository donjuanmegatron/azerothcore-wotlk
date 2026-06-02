// aa_class.cpp
//
// Sanctum AA System — Class tree hook-based AAs.
//
// Flat stat passives (Tactical Mastery, Hastened Attacks, Nature's Guidance) are
// in mod-aa-system.cpp ApplyAAStat. This file handles effects requiring combat hooks.
//
// IMPLEMENTED HERE:
//   5005  Punishing Blade      — Warrior: 2H auto-attacks 10/20/30% proc: +50/70/90% dmg
//   5006  Rend Mastery         — Warrior: +30% Rend DoT damage
//   5104  Improved Exorcism    — Paladin: +10/20/35% Exorcism damage
//   5106  Mandate of Heaven    — Paladin: +5/10/15% dmg vs non-targeting enemies
//   5109  Improved Consecration— Paladin: +15/25/40% Consecration damage
//   5112  Blessing of Austerity— Paladin: -2/4/6% all dmg taken while a Blessing is active
//   5121  Celestial Regen      — Paladin: 10/15/20% proc on hit: heal 3% max HP
//   5122  Celestial Hammer     — Paladin: 8/12/16% proc on hit: Holy dmg = 20% max HP
//   5123  Gift of the Keeper   — Paladin: on kill, restore 5/10/15% max mana
//   5201  Archery Mastery      — Hunter: +5/10/15% all damage done
//   5202  Double Bowshot       — Hunter: 5/10/15% proc on auto-attack: +60% extra dmg. 200ms ICD.
//   5204  Headshot             — Hunter: 3/6/10% proc on auto: instant kill humanoid/undead <35% HP
//   5205  Triple Arrow         — Hunter: 3/6/10% proc on auto: +40% extra dmg. 200ms ICD.
//   5206  Explosive Arrow      — Hunter: 5/10/15% proc on auto: AoE 50% dmg within 6 yd of target
//   5213  Auspice              — Hunter: +3/6/10% all damage done (stacks with Archery Mastery)
//   5217  Taste of Blood       — Hunter: +8/15/25% melee dmg when target is bleeding or poisoned
//   5221  Nature's Melody      — Hunter: +20/50/90 HP per 5s regen
//   5302  Backstab Focus       — Rogue: +8/15/25% Backstab and Sinister Strike damage
//   5309  Poison Mastery       — Rogue: +15/30/45% poison DoT damage
//   5316  Lacerate             — Rogue: 10/20/30% proc on hit: bleed DoT (AP*rank*5%, 4 ticks, 8s)
//   5318  Frenzy               — Rogue: below 35% HP, +8/15/25% melee attack speed
//   5322  Trauma               — Rogue: +10/20/35% bleed DoT damage
//   5425  Aura of the Pious    — Priest: +15/35/60 HP per 5s (player + active pet)
//   5501  Plague Lord          — Death Knight: +10/20/30% disease damage done
//   5502  Pestilence           — Death Knight: on kill, diseases jump to 1/2/3 nearby enemies
//   5504  Blood Rite           — Death Knight: on kill, restore 5/10/15% max HP
//   5506  Necrotic Touch       — DK: 10/20/30% proc on auto: shadow DoT (AP*5%, 3 ticks, 6s, max 3 stacks)
//   5508  Frost Rot            — Death Knight: +3/6/10% HB/FS/Obliterate when target has Frost Fever
//   5510  Contagion Drain      — Death Knight: 1%/2%/3% max HP/s while 2+ diseased enemies nearby
//   5513  Scourge Mastery      — Death Knight: +20% Scourge Strike damage
//   5515  Arctic Howl          — Death Knight: +10% Howling Blast damage
//   5603  Earthen Presence     — Shaman: -10/18/25% melee dmg from attackers (approx attack-speed debuff)
//   5602  Blood Tithe          — Shaman: Flame Shock ticks heal player 15/25/40% of damage
//   5609  Soul Harvest         — Shaman: on kill, restore 5/10/15% max mana
//   5807  Spirit Lash          — Warlock: every 3s deal shadow dmg = 15/25/40% SP to nearest enemy in 8 yd
//
// IN ApplyAAStat (mod-aa-system.cpp):
//   5003  Tactical Mastery   — +84 armor pen rating per rank
//   5210  Nature's Guidance  — +16 ranged hit rating per rank
//   5307  Hastened Attacks   — +164 melee haste rating per rank
//
// DEFERRED:
//   5122 Celestial Hammer stun — no clean 2s single-target stun spell available
//   All Druid AAs: need mod-druid-essence

#include "aa_runtime.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Unit.h"
#include "Item.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "Random.h"
#include "Creature.h"
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// File-local state
// ---------------------------------------------------------------------------
namespace
{
    static inline Player* AsPlayer(Unit* u)
    {
        return (u && u->IsPlayer()) ? u->ToPlayer() : nullptr;
    }

    template<typename T>
    static inline T Idx(uint8 rank) { return static_cast<T>(std::min<uint8>(rank, 3)); }

    // Punishing Blade ICD: playerGuid → last proc timestamp
    std::unordered_map<uint32, uint32> g_pbIcd;

    // Double Bowshot ICD
    std::unordered_map<uint32, uint32> g_dbsIcd;

    // Triple Arrow ICD
    std::unordered_map<uint32, uint32> g_triIcd;

    // Headshot ICD
    std::unordered_map<uint32, uint32> g_headIcd;

    // Explosive Arrow ICD
    std::unordered_map<uint32, uint32> g_expIcd;

    // Lacerate proc ICD and DoT state
    std::unordered_map<uint32, uint32> g_laceIcd;
    struct LacerateState { uint32 endMs; uint32 lastTickMs; uint32 tickDmg; };
    std::unordered_map<uint32, std::unordered_map<uint32, LacerateState>> g_lacerate; // playerGuid → victimLow → state

    // Necrotic Touch shadow DoT state
    struct NecroticState { uint32 endMs; uint32 lastTickMs; uint32 tickDmg; uint8 stacks; };
    std::unordered_map<uint32, std::unordered_map<uint32, NecroticState>> g_necrotic;

    // Frenzy — currently applied attack speed bonus pct (0 = not active)
    std::unordered_map<uint32, float> g_frenzyPct;

    // Spirit Lash 3s tick
    std::unordered_map<uint32, uint32> g_spiritLashTick;

    // Celestial Regen proc ICD
    std::unordered_map<uint32, uint32> g_celRegenIcd;

    // Celestial Hammer proc ICD
    std::unordered_map<uint32, uint32> g_celHammerIcd;

    // 5s HP regen tick tracker
    std::unordered_map<uint32, uint32> g_classRegenTick;

    // Contagion Drain 1s tick
    std::unordered_map<uint32, uint32> g_contDrainTick;

    static void ClearPlayerState(uint32 guid, Player* player = nullptr)
    {
        // Restore Frenzy speed modifier before clearing state
        if (player)
        {
            auto fIt = g_frenzyPct.find(guid);
            if (fIt != g_frenzyPct.end() && fIt->second > 0.0f)
                player->ApplyAttackTimePercentMod(BASE_ATTACK, fIt->second, false);
        }
        g_pbIcd.erase(guid);
        g_dbsIcd.erase(guid);
        g_triIcd.erase(guid);
        g_headIcd.erase(guid);
        g_expIcd.erase(guid);
        g_laceIcd.erase(guid);
        g_lacerate.erase(guid);
        g_necrotic.erase(guid);
        g_frenzyPct.erase(guid);
        g_spiritLashTick.erase(guid);
        g_celRegenIcd.erase(guid);
        g_celHammerIcd.erase(guid);
        g_classRegenTick.erase(guid);
        g_contDrainTick.erase(guid);
    }
}

// ---------------------------------------------------------------------------
// aa_class_unit — UnitScript for hook-based class AAs
// ---------------------------------------------------------------------------
class aa_class_unit : public UnitScript
{
public:
    aa_class_unit() : UnitScript("aa_class_unit", true,
    {
        UNITHOOK_MODIFY_MELEE_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
        UNITHOOK_ON_UNIT_UPDATE,
        UNITHOOK_ON_UNIT_DEATH,
    }) {}

    // -----------------------------------------------------------------------
    // ModifyMeleeDamage — attacker-side and victim-side class procs.
    // -----------------------------------------------------------------------
    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (damage == 0 || !target)
            return;

        // ── ATTACKER IS PLAYER ──────────────────────────────────────────────
        if (Player* player = AsPlayer(attacker))
        {
            uint32 guid = player->GetGUID().GetCounter();

            // Punishing Blade (Warrior) — 2H weapon only; 10/20/30% proc +50/70/90% dmg
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_PUNISHING_BLADE);
                if (rank > 0)
                {
                    Item* mh = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                    if (mh && mh->GetTemplate()->InventoryType == INVTYPE_2HWEAPON)
                    {
                        auto& stamp = g_pbIcd[guid];
                        if (GetMSTimeDiffToNow(stamp) >= 500u)
                        {
                            static const float chance[]   = { 0.0f, 10.0f, 20.0f, 30.0f };
                            static const float extraPct[] = { 0.0f,  0.50f,  0.70f,  0.90f };
                            if (roll_chance_f(chance[Idx<uint8>(rank)]))
                            {
                                damage += (uint32)(damage * extraPct[Idx<uint8>(rank)]);
                                stamp = getMSTime();
                            }
                        }
                    }
                }
            }

            // Mandate of Heaven (Paladin) — enemy is not targeting the player
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_MANDATE_OF_HEAVEN);
                if (rank > 0 && target->GetVictim() != player)
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Celestial Regeneration (Paladin) — 10/15/20% proc on hit: heal 3% max HP
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_CELESTIAL_REGENERATION);
                if (rank > 0)
                {
                    auto& stamp = g_celRegenIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 500u)
                    {
                        static const float chance[] = { 0.0f, 10.0f, 15.0f, 20.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]) && !player->IsFullHealth())
                        {
                            int32 heal = (int32)(player->GetMaxHealth() * 0.03f);
                            if (heal > 0)
                                player->ModifyHealth(heal);
                            stamp = getMSTime();
                        }
                    }
                }
            }

            // Celestial Hammer (Paladin) — 8/12/16% proc on hit: Holy dmg = 20% max HP
            // Stun effect deferred (no clean 2s single-target stun spell in 3.3.5a).
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_CELESTIAL_HAMMER);
                if (rank > 0)
                {
                    auto& stamp = g_celHammerIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 1000u)
                    {
                        static const float chance[] = { 0.0f, 8.0f, 12.0f, 16.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            uint32 holyDmg = player->GetMaxHealth() / 5u; // 20% max HP
                            Unit::DealDamage(player, target, holyDmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_HOLY, nullptr, false);
                            stamp = getMSTime();
                        }
                    }
                }
            }

            // Archery Mastery (Hunter) — +5/10/15% all damage done
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_ARCHERY_MASTERY);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Double Bowshot (Hunter) — 5/10/15% proc: add 60% extra damage. 200ms ICD.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_DOUBLE_BOWSHOT);
                if (rank > 0)
                {
                    auto& stamp = g_dbsIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 200u)
                    {
                        static const float chance[] = { 0.0f, 5.0f, 10.0f, 15.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            damage += (uint32)(damage * 0.60f);
                            stamp = getMSTime();
                        }
                    }
                }
            }

            // Triple Arrow (Hunter) — 3/6/10% proc: add 40% extra damage. 200ms ICD.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_TRIPLE_ARROW);
                if (rank > 0)
                {
                    auto& stamp = g_triIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 200u)
                    {
                        static const float chance[] = { 0.0f, 3.0f, 6.0f, 10.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            damage += (uint32)(damage * 0.40f);
                            stamp = getMSTime();
                        }
                    }
                }
            }

            // Headshot (Hunter) — 3/6/10% proc on auto: instant kill humanoid/undead below 35% HP
            // Does not work on elites or bosses.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_HEADSHOT);
                if (rank > 0 && target->GetHealthPct() < 35.0f)
                {
                    Creature const* cr = target->ToCreature();
                    if (cr && !cr->isElite() && !cr->IsDungeonBoss())
                    {
                        uint32 cType = cr->GetCreatureTemplate()->type;
                        if (cType == CREATURE_TYPE_HUMANOID || cType == CREATURE_TYPE_UNDEAD)
                        {
                            auto& stamp = g_headIcd[guid];
                            if (GetMSTimeDiffToNow(stamp) >= 500u)
                            {
                                static const float chance[] = { 0.0f, 3.0f, 6.0f, 10.0f };
                                if (roll_chance_f(chance[Idx<uint8>(rank)]))
                                {
                                    damage = target->GetHealth(); // lethal blow
                                    stamp = getMSTime();
                                }
                            }
                        }
                    }
                }
            }

            // Explosive Arrow (Hunter) — 5/10/15% proc on auto: AoE 50% of hit dmg within 6 yd of target
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_EXPLOSIVE_ARROW);
                if (rank > 0)
                {
                    auto& stamp = g_expIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 500u)
                    {
                        static const float chance[] = { 0.0f, 5.0f, 10.0f, 15.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            stamp = getMSTime();
                            uint32 aoeHit = damage / 2u;
                            // Collect enemies within 6 yd of target, excluding primary target
                            std::vector<Unit*> nearby;
                            for (Unit* atk : player->getAttackers())
                            {
                                if (atk != target && atk->IsAlive() && target->GetDistance(atk) <= 6.0f)
                                    nearby.push_back(atk);
                            }
                            for (Unit* u : nearby)
                                Unit::DealDamage(player, u, aoeHit, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                        }
                    }
                }
            }

            // Auspice (Hunter) — +3/6/10% all damage done (stacks with Archery Mastery)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_AUSPICE);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Taste of Blood (Hunter) — +8/15/25% melee dmg when target is bleeding or poisoned
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_TASTE_OF_BLOOD);
                if (rank > 0)
                {
                    bool triggered = target->HasAuraWithMechanic(1u << MECHANIC_BLEED);
                    if (!triggered)
                    {
                        for (auto const& pair : target->GetAppliedAuras())
                        {
                            if (pair.second->GetBase()->GetSpellInfo()->Dispel == DISPEL_POISON)
                            {
                                triggered = true;
                                break;
                            }
                        }
                    }
                    if (triggered)
                    {
                        static const float bonus[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // Lacerate (Rogue) — 10/20/30% proc on hit: apply bleed DoT (AP-based, 4 ticks, 8s)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_LACERATE);
                if (rank > 0)
                {
                    auto& stamp = g_laceIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 200u)
                    {
                        static const float chance[] = { 0.0f, 10.0f, 20.0f, 30.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            stamp = getMSTime();
                            uint32 victLow = target->GetGUID().GetCounter();
                            uint32 ap = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
                            // 5/10/15% AP per tick by rank
                            uint32 tickDmg = std::max(1u, (uint32)(ap * 0.05f * rank));
                            g_lacerate[guid][victLow] = LacerateState{getMSTime() + 8000u, getMSTime(), tickDmg};
                        }
                    }
                }
            }

            // Necrotic Touch (DK) — 10/20/30% proc on auto: shadow DoT (AP-based, 3 ticks, 6s, max 3 stacks)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_NECROTIC_TOUCH);
                if (rank > 0)
                {
                    static const float chance[] = { 0.0f, 10.0f, 20.0f, 30.0f };
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                    {
                        uint32 victLow = target->GetGUID().GetCounter();
                        auto& ns = g_necrotic[guid][victLow];
                        if (ns.stacks < 3)
                            ns.stacks++;
                        uint32 ap = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
                        ns.tickDmg = std::max(1u, (uint32)(ap * 0.04f * ns.stacks)); // 4/8/12% AP × stacks
                        ns.endMs = getMSTime() + 6000u;
                        if (ns.lastTickMs == 0)
                            ns.lastTickMs = getMSTime();
                    }
                }
            }
        } // end ATTACKER IS PLAYER

        // ── VICTIM IS PLAYER ────────────────────────────────────────────────
        if (Player* player = AsPlayer(target))
        {
            // Blessing of Austerity (Paladin) — -2/4/6% dmg taken while any Blessing is active
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_BLESSING_OF_AUSTERITY);
                if (rank > 0)
                {
                    static const std::unordered_set<uint32> s_blessings = {
                        // Blessing of Might (all ranks + Greater)
                        19740, 19834, 19835, 19836, 19837, 25291, 27140, 48932, 48933,
                        25782, 27141, 48934, 48935,
                        // Blessing of Wisdom (all ranks + Greater)
                        19742, 25290, 27142, 48936, 48937,
                        25894, 27143, 48938, 48939,
                        // Blessing of Kings + Greater
                        20217, 25898,
                        // Blessing of Sanctuary + Greater
                        20911, 25899,
                        // Blessing of Light + Greater
                        19977, 19978, 26890, 25890,
                    };
                    bool hasBlessing = false;
                    for (uint32 id : s_blessings)
                    {
                        if (player->HasAura(id)) { hasBlessing = true; break; }
                    }
                    if (hasBlessing)
                    {
                        static const float dr[] = { 0.0f, 0.02f, 0.04f, 0.06f };
                        damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }
            }

            // Earthen Presence (Shaman) — -10/18/25% melee dmg from attackers
            // Approximates the attack-speed slow as a flat DR on incoming melee.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_SHA_EARTHEN_PRESENCE);
                if (rank > 0)
                {
                    static const float dr[] = { 0.0f, 0.10f, 0.18f, 0.25f };
                    damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                }
            }
        } // end VICTIM IS PLAYER
    }

    // -----------------------------------------------------------------------
    // ModifySpellDamageTaken — class spell damage hooks.
    // -----------------------------------------------------------------------
    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage <= 0 || !target || !attacker || !spellInfo)
            return;

        // ── ATTACKER IS PLAYER ──────────────────────────────────────────────
        if (Player* player = AsPlayer(attacker))
        {
            // Mandate of Heaven (Paladin) — +5/10/15% spell dmg vs non-targeting enemy
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_MANDATE_OF_HEAVEN);
                if (rank > 0 && target->GetVictim() != player)
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Improved Exorcism (Paladin) — +10/20/35% Exorcism damage
            {
                static const std::unordered_set<uint32> s_exorcism = {
                    879,5614,5615,10312,10313,10314,27138,48800,48801
                };
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_IMPROVED_EXORCISM);
                if (rank > 0 && s_exorcism.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.35f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Improved Consecration (Paladin) — +15/25/40% Consecration damage
            {
                static const std::unordered_set<uint32> s_consecration = {
                    26573,20116,20922,20923,20924,48818,48819
                };
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_IMPROVED_CONSECRATION);
                if (rank > 0 && s_consecration.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Archery Mastery (Hunter) — +5/10/15% all damage done (spells/abilities)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_ARCHERY_MASTERY);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Auspice (Hunter) — +3/6/10% all damage done (spells/abilities)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_AUSPICE);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Backstab Focus (Rogue) — +8/15/25% Backstab and Sinister Strike damage
            {
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_BACKSTAB_FOCUS);
                if (rank > 0 &&
                    spellInfo->SpellFamilyName == SPELLFAMILY_ROGUE &&
                    (spellInfo->SpellFamilyFlags[0] & (0x800000u | 0x4u)))
                {
                    static const float bonus[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Frost Rot (Death Knight) — +3/6/10% HB/Frost Strike/Obliterate vs Frost Fever targets
            {
                static const std::unordered_set<uint32> s_frostAbl = {
                    49184,51411,51412,
                    49143,51416,51417,51418,51419,
                    49020,51423,51424,51425
                };
                uint8 rank = SanctumAA::GetRank(player, AA_DK_FROST_ROT);
                if (rank > 0 && s_frostAbl.count(spellInfo->Id) && target->HasAura(55095))
                {
                    static const float bonus[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Scourge Mastery (Death Knight) — +20% Scourge Strike damage
            {
                static const std::unordered_set<uint32> s_scourge = {
                    55090,55265,55270,55271
                };
                uint8 rank = SanctumAA::GetRank(player, AA_DK_SCOURGE_MASTERY);
                if (rank > 0 && s_scourge.count(spellInfo->Id))
                    damage += (int32)(damage * 0.20f);
            }

            // Arctic Howl (Death Knight) — +10% Howling Blast damage
            {
                static const std::unordered_set<uint32> s_howlingBlast = {
                    49184,51411,51412
                };
                uint8 rank = SanctumAA::GetRank(player, AA_DK_ARCTIC_HOWL);
                if (rank > 0 && s_howlingBlast.count(spellInfo->Id))
                    damage += (int32)(damage * 0.10f);
            }
        } // end ATTACKER IS PLAYER

        // ── VICTIM IS PLAYER ────────────────────────────────────────────────
        if (Player* player = AsPlayer(target))
        {
            // Blessing of Austerity (Paladin) — -2/4/6% all dmg taken while a Blessing is active
            {
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_BLESSING_OF_AUSTERITY);
                if (rank > 0)
                {
                    static const std::unordered_set<uint32> s_blessings = {
                        19740, 19834, 19835, 19836, 19837, 25291, 27140, 48932, 48933,
                        25782, 27141, 48934, 48935,
                        19742, 25290, 27142, 48936, 48937,
                        25894, 27143, 48938, 48939,
                        20217, 25898,
                        20911, 25899,
                        19977, 19978, 26890, 25890,
                    };
                    for (uint32 id : s_blessings)
                    {
                        if (player->HasAura(id))
                        {
                            static const float dr[] = { 0.0f, 0.02f, 0.04f, 0.06f };
                            damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                            break;
                        }
                    }
                }
            }
        } // end VICTIM IS PLAYER
    }

    // -----------------------------------------------------------------------
    // ModifyPeriodicDamageAurasTick
    // -----------------------------------------------------------------------
    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (damage == 0 || !spellInfo)
            return;

        Player* player = AsPlayer(attacker);
        if (!player)
            return;

        // Plague Lord (Death Knight) — +10/20/30% disease damage
        if (spellInfo->Dispel == DISPEL_DISEASE)
        {
            uint8 rank = SanctumAA::GetRank(player, AA_DK_PLAGUE_LORD);
            if (rank > 0)
            {
                static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
            }
        }

        // Poison Mastery (Rogue) — +15/30/45% poison DoT damage
        if (spellInfo->Dispel == DISPEL_POISON)
        {
            uint8 rank = SanctumAA::GetRank(player, AA_ROG_POISON_MASTERY);
            if (rank > 0)
            {
                static const float bonus[] = { 0.0f, 0.15f, 0.30f, 0.45f };
                damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
            }
        }

        // Trauma (Rogue) — +10/20/35% bleed DoT damage
        if (spellInfo->GetAllEffectsMechanicMask() & (1u << MECHANIC_BLEED))
        {
            uint8 rank = SanctumAA::GetRank(player, AA_ROG_TRAUMA);
            if (rank > 0)
            {
                static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.35f };
                damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
            }
        }

        // Rend Mastery (Warrior) — +30% Rend DoT damage
        {
            static const std::unordered_set<uint32> s_rend = {
                772,6546,6547,6548,11572,11574,25208,47465
            };
            uint8 rank = SanctumAA::GetRank(player, AA_WAR_REND_MASTERY);
            if (rank > 0 && s_rend.count(spellInfo->Id))
                damage += (uint32)(damage * 0.30f);
        }

        // Blood Tithe (Shaman) — Flame Shock ticks heal player 15/25/40% of damage
        {
            static const std::unordered_set<uint32> s_flameShock = {
                8050,8052,8053,10447,10448,29228,25457,49232,49233
            };
            uint8 rank = SanctumAA::GetRank(player, AA_SHA_BLOOD_TITHE);
            if (rank > 0 && s_flameShock.count(spellInfo->Id))
            {
                static const float pct[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                int32 healAmt = (int32)(damage * pct[Idx<uint8>(rank)]);
                if (healAmt > 0)
                    player->ModifyHealth(healAmt);
            }
        }
    }

    // -----------------------------------------------------------------------
    // OnUnitUpdate — periodic class AAs and DoT ticking.
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

        // ── 1s TICK BLOCK ───────────────────────────────────────────────────

        // Contagion Drain (Death Knight) — 1s tick: 1/2/3% max HP if 2+ diseased enemies
        {
            auto& cdTick = g_contDrainTick[guid];
            if (GetMSTimeDiffToNow(cdTick) >= 1000u)
            {
                cdTick = now;
                uint8 rank = SanctumAA::GetRank(player, AA_DK_CONTAGION_DRAIN);
                if (rank > 0 && !player->IsFullHealth())
                {
                    uint8 diseased = 0;
                    for (Unit* atk : player->getAttackers())
                    {
                        if (player->GetDistance(atk) > 15.0f)
                            continue;
                        for (auto const& pair : atk->GetAppliedAuras())
                        {
                            if (pair.second->GetBase()->GetCasterGUID() == player->GetGUID() &&
                                pair.second->GetBase()->GetSpellInfo()->Dispel == DISPEL_DISEASE)
                            {
                                ++diseased;
                                break;
                            }
                        }
                        if (diseased >= 2)
                            break;
                    }
                    if (diseased >= 2)
                    {
                        static const float pct[] = { 0.0f, 0.01f, 0.02f, 0.03f };
                        int32 healAmt = (int32)(player->GetMaxHealth() * pct[Idx<uint8>(rank)]);
                        if (healAmt > 0)
                            player->ModifyHealth(healAmt);
                    }
                }
            }
        }

        // Lacerate DoT ticking (Rogue) — physical bleed, 1 tick per 2s
        {
            auto laceIt = g_lacerate.find(guid);
            if (laceIt != g_lacerate.end() && !laceIt->second.empty())
            {
                std::vector<uint32> toErase;
                for (auto& [victLow, lstate] : laceIt->second)
                {
                    if (now > lstate.endMs) { toErase.push_back(victLow); continue; }
                    if (GetMSTimeDiffToNow(lstate.lastTickMs) < 2000u) continue;
                    lstate.lastTickMs = now;
                    // Locate victim in attacker list or current target
                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                    if (!victim)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                    }
                    if (!victim || !victim->IsAlive()) continue;
                    Unit::DealDamage(player, victim, lstate.tickDmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                }
                for (uint32 v : toErase) laceIt->second.erase(v);
            }
        }

        // Necrotic Touch DoT ticking (DK) — shadow, 1 tick per 2s
        {
            auto necIt = g_necrotic.find(guid);
            if (necIt != g_necrotic.end() && !necIt->second.empty())
            {
                std::vector<uint32> toErase;
                for (auto& [victLow, nstate] : necIt->second)
                {
                    if (now > nstate.endMs) { toErase.push_back(victLow); continue; }
                    if (GetMSTimeDiffToNow(nstate.lastTickMs) < 2000u) continue;
                    nstate.lastTickMs = now;
                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                    if (!victim)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                    }
                    if (!victim || !victim->IsAlive()) continue;
                    Unit::DealDamage(player, victim, nstate.tickDmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
                }
                for (uint32 v : toErase) necIt->second.erase(v);
            }
        }

        // Frenzy (Rogue) — below 35% HP: +8/15/25% melee attack speed
        {
            uint8 rank = SanctumAA::GetRank(player, AA_ROG_FRENZY);
            static const float pctByRank[] = { 0.0f, 8.0f, 15.0f, 25.0f };
            float needed = (rank > 0 && player->GetHealthPct() < 35.0f) ? pctByRank[Idx<uint8>(rank)] : 0.0f;
            float& current = g_frenzyPct[guid];
            if (needed != current)
            {
                if (current > 0.0f)
                    player->ApplyAttackTimePercentMod(BASE_ATTACK, current, false);
                if (needed > 0.0f)
                    player->ApplyAttackTimePercentMod(BASE_ATTACK, needed, true);
                current = needed;
            }
        }

        // Spirit Lash (Warlock) — every 3s, deal shadow dmg = 15/25/40% SP to nearest enemy in 8 yd
        {
            uint8 rank = SanctumAA::GetRank(player, AA_WRL_SPIRIT_LASH);
            if (rank > 0)
            {
                auto& stamp = g_spiritLashTick[guid];
                if (GetMSTimeDiffToNow(stamp) >= 3000u)
                {
                    stamp = now;
                    // Find nearest enemy within 8 yards
                    Unit* nearest = nullptr;
                    float nearestDist = 9.0f;
                    for (Unit* atk : player->getAttackers())
                    {
                        if (!atk->IsAlive()) continue;
                        float d = player->GetDistance(atk);
                        if (d <= 8.0f && d < nearestDist)
                        {
                            nearest = atk;
                            nearestDist = d;
                        }
                    }
                    if (!nearest)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->IsAlive() && player->GetDistance(v) <= 8.0f)
                            nearest = v;
                    }
                    if (nearest)
                    {
                        static const float spPct[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW);
                        uint32 lashDmg = std::max(1u, (uint32)(sp * spPct[Idx<uint8>(rank)]));
                        Unit::DealDamage(player, nearest, lashDmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
                    }
                }
            }
        }

        // ── 5s TICK BLOCK ───────────────────────────────────────────────────
        auto& tick = g_classRegenTick[guid];
        if (GetMSTimeDiffToNow(tick) < 5000u)
            return;
        tick = now;

        // Nature's Melody (Hunter) — +20/50/90 HP per 5s
        {
            uint8 rank = SanctumAA::GetRank(player, AA_HUN_NATURES_MELODY);
            if (rank > 0 && !player->IsFullHealth())
            {
                static const int32 healAmt[] = { 0, 20, 50, 90 };
                player->ModifyHealth(healAmt[Idx<uint8>(rank)]);
            }
        }

        // Aura of the Pious (Priest) — +15/35/60 HP per 5s; also heals active pet
        {
            uint8 rank = SanctumAA::GetRank(player, AA_PRI_AURA_OF_PIOUS);
            if (rank > 0)
            {
                static const int32 healAmt[] = { 0, 15, 35, 60 };
                int32 amt = healAmt[Idx<uint8>(rank)];
                if (!player->IsFullHealth())
                    player->ModifyHealth(amt);
                Pet* pet = player->GetPet();
                if (pet && !pet->IsFullHealth())
                    pet->ModifyHealth(amt);
            }
        }
    }

    // -----------------------------------------------------------------------
    // OnUnitDeath — clean up per-player state
    // -----------------------------------------------------------------------
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!unit->IsPlayer())
            return;
        ClearPlayerState(unit->GetGUID().GetCounter(), unit->ToPlayer());
    }
};

// ---------------------------------------------------------------------------
// aa_class_player — PlayerScript for on-kill class AAs
// ---------------------------------------------------------------------------
class aa_class_player : public PlayerScript
{
public:
    aa_class_player() : PlayerScript("aa_class_player") {}

    // -----------------------------------------------------------------------
    // OnPlayerCreatureKill — fires when the player kills any creature.
    // -----------------------------------------------------------------------
    void OnPlayerCreatureKill(Player* player, Creature* creature) override
    {
        // Blood Rite (Death Knight) — restore 5/10/15% max HP
        {
            uint8 rank = SanctumAA::GetRank(player, AA_DK_BLOOD_RITE);
            if (rank > 0 && !player->IsFullHealth())
            {
                static const float pct[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                int32 healAmt = (int32)(player->GetMaxHealth() * pct[Idx<uint8>(rank)]);
                if (healAmt > 0)
                    player->ModifyHealth(healAmt);
            }
        }

        // Soul Harvest (Shaman) — restore 5/10/15% max mana
        {
            uint8 rank = SanctumAA::GetRank(player, AA_SHA_SOUL_HARVEST);
            if (rank > 0)
            {
                static const float pct[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                uint32 maxMana = player->GetMaxPower(POWER_MANA);
                if (maxMana > 0)
                {
                    int32 gain = (int32)(maxMana * pct[Idx<uint8>(rank)]);
                    if (gain > 0)
                        player->ModifyPower(POWER_MANA, gain);
                }
            }
        }

        // Gift of the Keeper (Paladin) — restore 5/10/15% max mana
        {
            uint8 rank = SanctumAA::GetRank(player, AA_PAL_GIFT_OF_THE_KEEPER);
            if (rank > 0)
            {
                static const float pct[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                uint32 maxMana = player->GetMaxPower(POWER_MANA);
                if (maxMana > 0)
                {
                    int32 gain = (int32)(maxMana * pct[Idx<uint8>(rank)]);
                    if (gain > 0)
                        player->ModifyPower(POWER_MANA, gain);
                }
            }
        }

        // Pestilence (Death Knight) — diseases jump to nearest rank enemies within 15 yards
        {
            uint8 rank = SanctumAA::GetRank(player, AA_DK_PESTILENCE);
            if (rank > 0)
            {
                std::unordered_set<uint32> diseaseIds;
                for (auto const& pair : creature->GetAppliedAuras())
                {
                    AuraApplication const* app = pair.second;
                    if (app->GetBase()->GetCasterGUID() == player->GetGUID() &&
                        app->GetBase()->GetSpellInfo()->Dispel == DISPEL_DISEASE)
                    {
                        diseaseIds.insert(pair.first);
                    }
                }
                if (!diseaseIds.empty())
                {
                    uint8 jumped = 0;
                    for (Unit* atk : player->getAttackers())
                    {
                        if (jumped >= rank)
                            break;
                        if (atk == creature || player->GetDistance(atk) > 15.0f)
                            continue;
                        for (uint32 diseaseId : diseaseIds)
                            player->CastSpell(atk, diseaseId, true);
                        ++jumped;
                    }
                }
            }
        }
    }

    // Clean up state on logout — pass player pointer to restore Frenzy speed mod
    void OnPlayerLogout(Player* player) override
    {
        ClearPlayerState(player->GetGUID().GetCounter(), player);
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void AddSC_aa_class()
{
    new aa_class_unit();
    new aa_class_player();
}
