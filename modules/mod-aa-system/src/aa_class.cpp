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
//   5215  Poison Arrow         — Hunter: always-proc nature DoT on auto (AP×5%×rank/5 per 2s tick, 10s). 200ms ICD.
//   5216  Burning Arrow        — Hunter: 10/20/30% proc fire DoT (AP×5% per 2s tick, 6s). 500ms ICD.
//   5217  Taste of Blood       — Hunter: +8/15/25% melee dmg when target is bleeding or poisoned
//   5221  Nature's Melody      — Hunter: +20/50/90 HP per 5s regen
//   5226  Improved Shots       — Hunter: +3/6/10% damage to activated Hunter shots
//   5242  Marked for Death     — Hunter: +5/10/15% dmg to targets with Hunter's Mark (melee + spell)
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
//   5505  Unholy Guard         — Death Knight: absorbs 5/8/12% melee+spell dmg via Runic Power spend
//   5507  Iron Shell           — Death Knight: PARTIAL -10/15/20% DR while AMS/Bone Shield active; R3 CD reduction
//   5513  Scourge Mastery      — Death Knight: +15/25/35% Scourge Strike dmg; R2+ applies both diseases on hit
//   5514  Rune Blade Mastery   — Death Knight: PARTIAL +5/10/15% dmg while Dancing Rune Weapon active
//   5515  Arctic Howl          — Death Knight: Howling Blast +20/35/50% dmg, guaranteed crit-magnitude, R2+ spreads FF+BP to all targets hit (AoE spike)
//   5516  Battle Frenzy        — Death Knight: PARTIAL Hysteria CD reduction; infinite duration stubbed
//   5517  Deathchill Mastery   — Death Knight: STUB — Deathchill spell ID not found in mod-dk-rework
//   5518  Plague's End         — Death Knight: on kill, 15% HP heal + spread diseases to 2 nearby
//   5519  Final Rune           — Death Knight: cheat-death, 15% HP survive + 20% HoT, 3min CD
//   5520  Virulent Plague      — Death Knight: PARTIAL Nature DoT queued on Plague Strike, +4s rider stubbed
//   5524  Soul Abrasion        — Death Knight: flat 5/8/12% HP heal + 10/15/20 RP on Death Strike cast
//   5526  Improved Harm Touch  — Death Knight: +15/30/45% Death Coil damage (reframed from resist-reduce)
//   5603  Earthen Presence     — Shaman: -10/18/25% melee dmg from attackers (approx attack-speed debuff)
//   5602  Blood Tithe          — Shaman: Flame Shock ticks heal player 15/25/40% of damage
//   5609  Soul Harvest         — Shaman: on kill, restore 5/10/15% max mana
//   5807  Spirit Lash          — Warlock: every 3s deal shadow dmg = 15/25/40% SP to nearest enemy in 8 yd
//   5808  Umbral Leech         — Warlock: Hellfire self-ticks heal player 1/2/3% max HP per tick
//   5809  Burning Soul         — Warlock: 20/35/50% incoming damage drained from mana instead
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
#include "Spell.h"
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

    // Crusader's Might (5100) proc ICD
    std::unordered_map<uint32, uint32> g_cmIcd;

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

    // Poison Arrow (Hunter) — nature DoT ICD and state
    std::unordered_map<uint32, uint32> g_poisonArrowIcd;
    struct PoisonArrowState { uint32 endMs; uint32 lastTickMs; uint32 tickDmg; };
    std::unordered_map<uint32, std::unordered_map<uint32, PoisonArrowState>> g_poisonArrow; // playerGuid → victimLow → state

    // Burning Arrow (Hunter) — fire DoT ICD and state
    std::unordered_map<uint32, uint32> g_burningArrowIcd;

    // Go for the Throat (Hunter) — pet bite ICD
    std::unordered_map<uint32, uint32> g_gftIcd;
    struct BurningArrowState { uint32 endMs; uint32 lastTickMs; uint32 tickDmg; };
    std::unordered_map<uint32, std::unordered_map<uint32, BurningArrowState>> g_burningArrow; // playerGuid → victimLow → state

    // Frenzy — currently applied attack speed bonus pct (0 = not active)
    std::unordered_map<uint32, float> g_frenzyPct;

    // Tricks (5324) proc ICD
    std::unordered_map<uint32, uint32> g_tricksIcd;

    // Rogue Poison system (5339 Poison Master, 5330 Imp Rupture, 5336 Imp Mutilate, 5338 Invigoration, 5342 Leeching Toxins)
    struct RoguePoison { uint8 stacks = 0; uint32 endMs = 0; uint32 lastTickMs = 0; uint32 tickDmg = 0; };
    // playerGuid → victimLow → state
    std::unordered_map<uint32, std::unordered_map<uint32, RoguePoison>> g_roguePoison;
    // last time player queued a poison application (for Invigoration window)
    std::unordered_map<uint32, uint32> g_poisonAppliedMs;
    // Invigoration energy tick tracker
    std::unordered_map<uint32, uint32> g_invigorTick;

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

    // ── Shaman: Scorched Earth (5614) — queued Fire DoT from Lava Burst ────
    struct ScorchedState { uint32 endMs = 0; uint32 lastTickMs = 0; uint32 tickDmg = 0; };
    std::unordered_map<uint32, std::unordered_map<uint32, ScorchedState>> g_scorchedEarth; // playerGuid → victimLow → state

    // ── Shaman: Ancestral Bulwark (5619) — Earth Shield queue ───────────────
    // playerGuid → list of targetLow GUIDs to apply Earth Shield on next safe tick
    std::unordered_map<uint32, std::vector<uint32>> g_earthShieldQueue; // playerGuid → {targetLows}

    // ── Shaman: Ancestral Guard (5605) — post-self-heal absorb shield ───────
    struct AncestralGuardAbsorb { int32 absorb = 0; uint32 expireMs = 0; };
    std::unordered_map<uint32, AncestralGuardAbsorb> g_ancestralGuardAbsorb; // playerGuid → state

    // ── Shaman: Ghost Strike (5617) — periodic Nature strike ────────────────
    std::unordered_map<uint32, uint32> g_ghostStrikeTick; // playerGuid → last tick ms

    // ── Shaman: Lightning Rod (5615) — post-CL periodic bounce window ───────
    struct LightningRodState { uint32 expireMs = 0; uint32 lastFireMs = 0; uint32 targetLow = 0; uint8 rank = 0; };
    std::unordered_map<uint32, LightningRodState> g_lightningRod; // playerGuid → state

    // ── Shaman: Elemental Accord (5620) — per-5s active-totem buff tracking ─
    // Tracks currently applied totem bonus stat amounts so we can remove them when totems drop.
    // Stored as flat AP added; recalculated every 5s.
    struct ElementalAccordState { int32 appliedAP = 0; uint32 lastTickMs = 0; };
    std::unordered_map<uint32, ElementalAccordState> g_elementalAccord; // playerGuid → state

    // ── Shaman: Alpha Pack (5610) — Feral Spirit CD tracking ────────────────
    // ModifySpellCooldown on cast handles CD reduction; wolf-buff tracking done in aa_pet.cpp.
    // (no extra state needed here)

    // ── Death Knight: Virulent Plague (5520) — queued Nature DoT ───────────
    // playerGuid → victimLow → {endMs, lastTickMs, tickDmg}
    struct VirulentState { uint32 endMs = 0; uint32 lastTickMs = 0; uint32 tickDmg = 0; };
    std::unordered_map<uint32, std::unordered_map<uint32, VirulentState>> g_virulentPlague;
    // Virulent Plague pending application queue: playerGuid → victimLow (apply on next OnUnitUpdate)
    std::unordered_map<uint32, uint32> g_virulentQueue; // playerGuid → victimLow

    // ── Death Knight: Ghoul Infestation (5521) — queued disease apply ───────
    // playerGuid → victimLow  (apply ONE alternating disease on next OnUnitUpdate)
    std::unordered_map<uint32, uint32> g_ghoulInfestQueue; // playerGuid → victimLow
    // Track which disease to apply next (alternates per player)
    std::unordered_map<uint32, bool> g_ghoulInfestToggle; // false=Frost Fever, true=Blood Plague

    // ── Death Knight: Scourge Mastery R2+ — apply BOTH diseases on next tick ──
    // playerGuid → victimLow  (apply Frost Fever + Blood Plague on next safe tick)
    std::unordered_map<uint32, uint32> g_scourgeDiseasesQueue; // playerGuid → victimLow

    // ── Death Knight: Arctic Howl R2+ (5515) — AoE disease spread ─────────────
    // Howling Blast hits multiple targets, so this is a MULTI-victim queue
    // (one entry per target hit that tick). Drained in OnUnitUpdate → casts
    // Frost Fever + Blood Plague on each. playerGuid → list of victimLow.
    std::unordered_map<uint32, std::vector<uint32>> g_howlSpreadQueue;

    static void ClearPlayerState(uint32 guid, Player* player = nullptr)
    {
        // Restore Frenzy speed modifier before clearing state
        if (player)
        {
            auto fIt = g_frenzyPct.find(guid);
            if (fIt != g_frenzyPct.end() && fIt->second > 0.0f)
                player->ApplyAttackTimePercentMod(BASE_ATTACK, fIt->second, false);
        }
        g_cmIcd.erase(guid);
        g_pbIcd.erase(guid);
        g_dbsIcd.erase(guid);
        g_triIcd.erase(guid);
        g_headIcd.erase(guid);
        g_expIcd.erase(guid);
        g_laceIcd.erase(guid);
        g_lacerate.erase(guid);
        g_necrotic.erase(guid);
        g_poisonArrowIcd.erase(guid);
        g_poisonArrow.erase(guid);
        g_burningArrowIcd.erase(guid);
        g_burningArrow.erase(guid);
        g_gftIcd.erase(guid);
        g_frenzyPct.erase(guid);
        g_spiritLashTick.erase(guid);
        g_celRegenIcd.erase(guid);
        g_celHammerIcd.erase(guid);
        g_classRegenTick.erase(guid);
        g_contDrainTick.erase(guid);
        // Rogue
        g_tricksIcd.erase(guid);
        g_roguePoison.erase(guid);
        g_poisonAppliedMs.erase(guid);
        g_invigorTick.erase(guid);
        // Death Knight
        g_virulentPlague.erase(guid);
        g_virulentQueue.erase(guid);
        g_ghoulInfestQueue.erase(guid);
        g_ghoulInfestToggle.erase(guid);
        g_scourgeDiseasesQueue.erase(guid);
        g_howlSpreadQueue.erase(guid);
        // Shaman
        g_scorchedEarth.erase(guid);
        g_earthShieldQueue.erase(guid);
        g_ancestralGuardAbsorb.erase(guid);
        g_ghostStrikeTick.erase(guid);
        g_lightningRod.erase(guid);
        // Elemental Accord: remove any lingering stat bonus on death (player arg required)
        // Done in OnUnitDeath after ClearPlayerState is called with the player pointer.
    }
}

// ---------------------------------------------------------------------------
// File-local helper — apply / refresh Rogue poison stacks on a victim.
// Called from multiple hooks in this file (Improved Rupture, Improved Mutilate,
// Poison Master) so it lives here as a file-local static function.
// ---------------------------------------------------------------------------
static void QueueRoguePoison(Player* player, Unit* victim, uint32 weaponCount)
{
    if (!player || !victim) return;
    uint32 guid     = player->GetGUID().GetCounter();
    uint32 victLow  = victim->GetGUID().GetCounter();
    uint32 now      = getMSTime();

    // Stack cap: R3 Poison Master raises it to 6, otherwise 5.
    uint8 capStacks = (SanctumAA::GetRank(player, AA_ROG_POISON_MASTER) >= 3) ? 6 : 5;

    auto& entry = g_roguePoison[guid][victLow];
    // Reset expired state
    if (entry.stacks > 0 && now > entry.endMs)
        entry = RoguePoison{};

    // Add stacks, capped
    entry.stacks = (uint8)std::min<uint32>(capStacks, (uint32)entry.stacks + weaponCount);
    entry.endMs  = now + 12000u;
    if (entry.lastTickMs == 0)
        entry.lastTickMs = now;

    // Recalculate tick damage: AP × 0.04 × stacks × Poison Mastery bonus
    uint32 ap = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
    float  pm = 1.0f;
    uint8  pmRank = SanctumAA::GetRank(player, AA_ROG_POISON_MASTERY);
    if (pmRank > 0)
    {
        static const float pb[] = { 0.0f, 0.15f, 0.30f, 0.45f };
        pm = 1.0f + pb[std::min<uint8>(pmRank, 3)];
    }
    entry.tickDmg = std::max(1u, (uint32)(ap * 0.04f * entry.stacks * pm));

    // Record last application time for Invigoration window
    g_poisonAppliedMs[guid] = now;
}

// ---------------------------------------------------------------------------
// Exported helper: aa_pet.cpp calls this from ModifyMeleeDamage to schedule
// a Ghoul Infestation disease apply (CastSpell) for the next safe OnUnitUpdate tick.
// ---------------------------------------------------------------------------
void SanctumAA_QueueGhoulInfest(uint32 playerGuid, uint32 victimLow)
{
    g_ghoulInfestQueue[playerGuid] = victimLow;
}

// ---------------------------------------------------------------------------
// Exported helper: aa_combat_modifiers.cpp calls this from ModifyHealReceived
// to schedule Earth Shield application (CastSpell must NOT run inside that hook).
// Drained in aa_class_unit::OnUnitUpdate below.
// ---------------------------------------------------------------------------
void SanctumAA_QueueEarthShield(uint32 playerGuid, uint32 targetLow)
{
    g_earthShieldQueue[playerGuid].push_back(targetLow);
}

// ---------------------------------------------------------------------------
// Exported helper: aa_combat_modifiers.cpp calls this from ModifyHealReceived
// to set the Ancestral Guard absorb shield state.
// The g_ancestralGuardAbsorb map is owned by aa_class.cpp and consumed in the
// damage-taken hooks in this same file.
// ---------------------------------------------------------------------------
void SanctumAA_ApplyAncestralGuard(uint32 playerGuid, int32 amount, uint32 durationMs)
{
    auto& st = g_ancestralGuardAbsorb[playerGuid];
    // Add new shield on top of existing (stacks up to next 8s window)
    st.absorb    += amount;
    st.expireMs   = getMSTime() + durationMs;
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

            // Poison Arrow (Hunter) — always-proc nature DoT on auto-attack. 200ms ICD. 5 ticks / 10s.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_POISON_ARROW);
                if (rank > 0)
                {
                    auto& stamp = g_poisonArrowIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 200u)
                    {
                        stamp = getMSTime();
                        uint32 victLow = target->GetGUID().GetCounter();
                        uint32 ap = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
                        // tickDmg = AP × 0.05 × rank / 5  (spread over 5 ticks)
                        uint32 tickDmg = std::max(1u, (uint32)(ap * 0.05f * rank / 5.0f));
                        g_poisonArrow[guid][victLow] = PoisonArrowState{getMSTime() + 10000u, getMSTime(), tickDmg};
                    }
                }
            }

            // Burning Arrow (Hunter) — 10/20/30% proc on auto-attack: fire DoT. 500ms ICD. 3 ticks / 6s.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_BURNING_ARROW);
                if (rank > 0)
                {
                    auto& stamp = g_burningArrowIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 500u)
                    {
                        static const float chance[] = { 0.0f, 10.0f, 20.0f, 30.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            stamp = getMSTime();
                            uint32 victLow = target->GetGUID().GetCounter();
                            uint32 ap = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
                            uint32 tickDmg = std::max(1u, (uint32)(ap * 0.05f));
                            g_burningArrow[guid][victLow] = BurningArrowState{getMSTime() + 6000u, getMSTime(), tickDmg};
                        }
                    }
                }
            }

            // Marked for Death (Hunter) — +5/10/15% dmg to targets with Hunter's Mark
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_MARKED_FOR_DEATH);
                if (rank > 0)
                {
                    static const std::unordered_set<uint32> s_huntersMark = { 1130, 14323, 14324, 14325, 53338 };
                    bool marked = false;
                    for (uint32 id : s_huntersMark)
                        if (target->HasAura(id)) { marked = true; break; }
                    if (marked)
                    {
                        static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // Beast Synergy (Hunter) — +3/6/10% damage while player has a living pet or guardian.
            // Checks the real pet slot (GetPet) and the guardian slot (GetGuardianPet/UNIT_FIELD_SUMMON).
            // mod-pet-systems guardians stored only in m_Controlled (no public API) are not detected
            // here — the real Hunter beast covers the primary use case.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_BEAST_SYNERGY);
                if (rank > 0)
                {
                    bool hasPetAlive = false;
                    Pet* mainPet = player->GetPet();
                    if (mainPet && mainPet->IsAlive())
                        hasPetAlive = true;
                    if (!hasPetAlive)
                    {
                        Unit* guardian = player->GetGuardianPet();
                        if (guardian && guardian->IsAlive())
                            hasPetAlive = true;
                    }
                    if (hasPetAlive)
                    {
                        static const float bonus[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // Coordinated Assault (Hunter) — +5/10/15% damage to target the pet is currently attacking
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_COORDINATED_ASSAULT);
                if (rank > 0)
                {
                    Pet* pet = player->GetPet();
                    if (pet && pet->IsAlive() && pet->GetVictim() == target)
                    {
                        static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // Go for the Throat (Hunter) — 10/20/30% proc: pet bites for 40/60/80% of the hit. 500ms ICD.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_GO_FOR_THE_THROAT);
                if (rank > 0)
                {
                    Pet* pet = player->GetPet();
                    if (pet && pet->IsAlive())
                    {
                        auto& stamp = g_gftIcd[guid];
                        if (GetMSTimeDiffToNow(stamp) >= 500u)
                        {
                            static const float chance[]  = { 0.0f, 10.0f, 20.0f, 30.0f };
                            static const float bitePct[] = { 0.0f,  0.40f,  0.60f,  0.80f };
                            if (roll_chance_f(chance[Idx<uint8>(rank)]))
                            {
                                uint32 bite = (uint32)(damage * bitePct[Idx<uint8>(rank)]);
                                if (bite > 0)
                                    Unit::DealDamage(pet, target, bite, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                                stamp = getMSTime();
                            }
                        }
                    }
                }
            }

            // Ambidexterity (5301) — reduce off-hand damage penalty (off-hand swings only)
            // The hit-rating portion lives in ApplyAAStat. Here we add a bonus to partially
            // offset the normal 50% off-hand damage penalty.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_AMBIDEXTERITY);
                if (rank > 0 && player->haveOffhandWeapon())
                {
                    static const float bonus[] = { 0.0f, 0.04f, 0.08f, 0.12f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Tricks (5324) — 8/15/25% chance free strike for 75% bonus damage. 200ms ICD.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_TRICKS);
                if (rank > 0)
                {
                    auto& stamp = g_tricksIcd[guid];
                    if (GetMSTimeDiffToNow(stamp) >= 200u)
                    {
                        static const float chance[] = { 0.0f, 8.0f, 15.0f, 25.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            damage += (uint32)(damage * 0.75f);
                            stamp = getMSTime();
                        }
                    }
                }
            }

            // ── Priest Yaulp (5405) — +10/20/30% melee damage while active window ──
            // Speed is handled at activation (ApplyAttackTimePercentMod).
            // Damage bonus is applied here per-swing while the window is open.
            {
                extern bool SanctumAA_PriestYaulpActive(uint32 guid, uint8& outRank);
                uint8 yRank = 0;
                if (SanctumAA_PriestYaulpActive(guid, yRank))
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(yRank)]);
                }
            }

            // Rune Blade Mastery (5514) — PARTIAL: while Dancing Rune Weapon (49028) aura is active,
            // +5/10/15% melee damage done.
            // DRW duration/cost/count modifications are not cleanly moddable in 3.3.5a; stubbed.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_RUNE_BLADE);
                if (rank > 0 && player->HasAura(49028u))
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Weapon Attunement (5607) — PARTIAL STUB ───────────────────────────
            // Catalog: +5/10/15% extra chance to proc weapon enchant (MH+OH).
            // "Force-increasing weapon imbue proc chance" requires hooking into the
            // enchant proc probability check, which is deep in the spell proc code and
            // not exposed as a hook in 3.3.5a without core modification.
            // APPROXIMATION: each swing has a 5/10/15% chance to deal an extra Nature
            // damage hit = 40% AP, simulating the feel of a weapon enchant going off.
            // No ICD — mirrors the "extra proc chance" intent.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_SHA_WEAPON_ATTUNEMENT);
                if (rank > 0)
                {
                    static const float chance[] = { 0.0f, 5.0f, 10.0f, 15.0f };
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                    {
                        uint32 ap2 = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
                        uint32 procDmg2 = std::max(1u, (uint32)(ap2 * 0.40f));
                        SanctumAA_DealVisibleDamage(player, target, procDmg2, SPELL_SCHOOL_MASK_NATURE);
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

            // Unholy Guard (DK, 5505) — absorbs 5/8/12% of incoming melee damage, spending Runic Power.
            // NOTE: secondary/tertiary DKs get RP topped up via a hidden pool in mod-multiclass, so
            // GetPower(POWER_RUNIC_POWER) works regardless of the character's primary power type.
            // 1 Runic Power absorbs approximately 1 point of damage (RP pool caps absorb amount).
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_UNHOLY_GUARD);
                if (rank > 0 && damage > 0)
                {
                    uint32 rpAvail = player->GetPower(POWER_RUNIC_POWER);
                    if (rpAvail > 0)
                    {
                        static const float pct[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                        uint32 absorb = std::min(static_cast<uint32>(damage * pct[Idx<uint8>(rank)]),
                                                 rpAvail);
                        if (absorb > 0)
                        {
                            damage -= absorb;
                            player->ModifyPower(POWER_RUNIC_POWER, -static_cast<int32>(absorb));
                        }
                    }
                }
            }

            // Iron Shell (5507) — PARTIAL: while AMS (48707) or Bone Shield (49222) is active,
            // apply -10/15/20% DR on incoming melee damage.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_IRON_SHELL);
                if (rank > 0 && damage > 0 &&
                    (player->HasAura(48707u) || player->HasAura(49222u)))
                {
                    static const float dr[] = { 0.0f, 0.10f, 0.15f, 0.20f };
                    damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                }
            }

            // Ancestral Guard (5605) — consume absorb shield on incoming melee hit
            {
                uint32 vGuid2 = player->GetGUID().GetCounter();
                auto it = g_ancestralGuardAbsorb.find(vGuid2);
                if (it != g_ancestralGuardAbsorb.end() && it->second.absorb > 0 && damage > 0)
                {
                    if (getMSTime() > it->second.expireMs)
                    {
                        it->second.absorb = 0;
                    }
                    else
                    {
                        uint32 absorbed = std::min((uint32)it->second.absorb, damage);
                        damage -= absorbed;
                        it->second.absorb -= (int32)absorbed;
                    }
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

            // Divine Storm Mastery (5103) — +15/25/40% Divine Storm damage
            {
                static const std::unordered_set<uint32> s_divineStorm = { 53385 };
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_DIVINE_STORM_MASTERY);
                if (rank > 0 && s_divineStorm.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Improved Avenger's Shield (5110) — +20% Avenger's Shield damage;
            // also applies a damage-dealt-reduction debuff to the target for 8s.
            {
                static const std::unordered_set<uint32> s_avengerShield = {
                    31935, 32699, 32700, 48826, 48827
                };
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_IMPROVED_AVENGERS_SHIELD);
                if (rank > 0 && s_avengerShield.count(spellInfo->Id) && target)
                {
                    damage += (int32)(damage * 0.20f);
                    // Apply debuff to target
                    static const float dr[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                    extern void SanctumAA_ApplyAvengerDebuff(uint32 playerGuid, uint32 targetGuid, float dr, uint32 durationMs);
                    SanctumAA_ApplyAvengerDebuff(player->GetGUID().GetCounter(), target->GetGUID().GetCounter(),
                                                  dr[Idx<uint8>(rank)], 8000u);
                }
            }

            // Fist of Reckoning (5111) — +30/50/80% Hand of Reckoning damage
            {
                static const std::unordered_set<uint32> s_hor = { 62124 };
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_FIST_OF_RECKONING);
                if (rank > 0 && s_hor.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.30f, 0.50f, 0.80f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Holy Wrath Mastery (5118) — +20/35/50% Holy Wrath damage
            {
                static const std::unordered_set<uint32> s_holyWrath = {
                    2812, 10312, 10313, 27139, 48816, 48817
                };
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_HOLY_WRATH_MASTERY);
                if (rank > 0 && s_holyWrath.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Crusader's Might (5100) — 10/18/25% chance on Crusader Strike to hit a 2nd time for full holy dmg
            {
                static const std::unordered_set<uint32> s_cs = {
                    35395, 35396, 35397, 35398, 35399
                };
                uint8 rank = SanctumAA::GetRank(player, AA_PAL_CRUSADERS_MIGHT);
                if (rank > 0 && s_cs.count(spellInfo->Id) && target)
                {
                    auto& stamp = g_cmIcd[player->GetGUID().GetCounter()];
                    if (GetMSTimeDiffToNow(stamp) >= 500u)
                    {
                        static const float chance[] = { 0.0f, 10.0f, 18.0f, 25.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            // Deal holy damage equal to current strike damage
                            Unit::DealDamage(player, target, (uint32)damage, nullptr,
                                             DIRECT_DAMAGE, SPELL_SCHOOL_MASK_HOLY, nullptr, false);
                            stamp = getMSTime();
                        }
                    }
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

            // Beast Synergy (Hunter) — +3/6/10% spell/ability damage while pet or guardian is alive.
            // Same pet detection logic as in ModifyMeleeDamage above (GetPet + GetGuardianPet).
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_BEAST_SYNERGY);
                if (rank > 0)
                {
                    bool hasPetAlive = false;
                    Pet* mainPet = player->GetPet();
                    if (mainPet && mainPet->IsAlive())
                        hasPetAlive = true;
                    if (!hasPetAlive)
                    {
                        Unit* guardian = player->GetGuardianPet();
                        if (guardian && guardian->IsAlive())
                            hasPetAlive = true;
                    }
                    if (hasPetAlive)
                    {
                        static const float bonus[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // Coordinated Assault (Hunter) — +5/10/15% spell/ability damage to target the pet is attacking
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_COORDINATED_ASSAULT);
                if (rank > 0)
                {
                    Pet* pet = player->GetPet();
                    if (pet && pet->IsAlive() && pet->GetVictim() == target)
                    {
                        static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // Improved Shots (Hunter) — +3/6/10% damage to Hunter activated shots
            {
                static const std::unordered_set<uint32> s_hunterShots = {
                    // Arcane Shot
                    3044,14281,14282,14283,14284,14285,14286,14287,27019,49044,49045,
                    // Multi-Shot
                    2643,14288,14289,14290,25294,27021,49047,49048,
                    // Aimed Shot
                    19434,20900,20901,20902,20903,20904,27065,49049,49050,
                    // Steady Shot
                    34120,49051,49052,
                    // Explosive Shot
                    53301,60051,60052,60053,
                    // Black Arrow
                    3674,63668,63669,63670,63671
                };
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_IMPROVED_SHOTS);
                if (rank > 0 && s_hunterShots.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Marked for Death (Hunter) — +5/10/15% spell dmg to targets with Hunter's Mark
            {
                uint8 rank = SanctumAA::GetRank(player, AA_HUN_MARKED_FOR_DEATH);
                if (rank > 0)
                {
                    static const std::unordered_set<uint32> s_huntersMark = { 1130, 14323, 14324, 14325, 53338 };
                    bool marked = false;
                    for (uint32 id : s_huntersMark)
                        if (target->HasAura(id)) { marked = true; break; }
                    if (marked)
                    {
                        static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
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

            // Chaotic Stab (5341) — Backstab from front: apply rank-based damage scaling
            // Positional strip (SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET) handled in OnWorldStartup.
            // This hook applies the damage multiplier when attacking from in front.
            {
                // All WotLK Backstab ranks
                static const std::unordered_set<uint32> s_backstab = {
                    53, 2589, 2590, 2591, 8721, 11279, 11280, 11281, 25300, 26863, 48656, 48657
                };
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_CHAOTIC_STAB);
                if (rank > 0 && s_backstab.count(spellInfo->Id) && target)
                {
                    // Only apply penalty if attacking from the front (not behind)
                    // At R3 the penalty disappears entirely (multiply by 1.0 = no change).
                    if (target->isInBack(player))
                    {
                        // Already behind — no Chaotic Stab interaction needed
                    }
                    else
                    {
                        // Front/side attack: apply rank-scaled multiplier
                        // R1: 75%, R2: 88%, R3: 100% (no penalty)
                        static const float mult[] = { 0.0f, 0.75f, 0.88f, 1.00f };
                        damage = (int32)(damage * mult[Idx<uint8>(rank)]);
                    }
                }
            }

            // Debilitation (5332) — finisher +8/15/25% damage vs targets with Expose Armor or Rupture
            {
                // WotLK Rogue finisher spell IDs
                static const std::unordered_set<uint32> s_finishers = {
                    // Eviscerate (all ranks)
                    2098, 6760, 6761, 6762, 8623, 8624, 11299, 11300, 26865, 48667, 48668,
                    // Rupture (all ranks)
                    1943, 8639, 8640, 11273, 11274, 11275, 26867, 48671, 48672,
                    // Kidney Shot (all ranks)
                    408, 8643, 11274, 11275,
                    // Envenom (all ranks)
                    32645, 32684, 41487, 41488,
                    // Slice and Dice (all ranks)
                    5171, 6774,
                    // Expose Armor (all ranks)
                    8647, 8649, 8650, 11197, 11198, 26866, 48668, 48669
                };
                // Expose Armor aura IDs on target
                static const std::unordered_set<uint32> s_exposeArmor = {
                    8647, 8649, 8650, 11197, 11198, 26866, 48668, 48669
                };
                // Rupture DoT aura IDs on target
                static const std::unordered_set<uint32> s_ruptureAura = {
                    1943, 8639, 8640, 11273, 11274, 11275, 26867, 48671, 48672
                };
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_DEBILITATION);
                if (rank > 0 && s_finishers.count(spellInfo->Id) && target)
                {
                    static const float bonus[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                    float b = bonus[Idx<uint8>(rank)];
                    bool hasExposeArmor = false;
                    bool hasRupture     = false;
                    for (uint32 id : s_exposeArmor)
                        if (target->HasAura(id)) { hasExposeArmor = true; break; }
                    for (uint32 id : s_ruptureAura)
                        if (target->HasAura(id)) { hasRupture = true; break; }
                    if (hasExposeArmor)
                        damage += (int32)(damage * b);
                    if (hasRupture)
                        damage += (int32)(damage * b);
                }
            }

            // Assassin's Mark (5315) — spell/ability bonus is applied once, in
            // aa_combat_modifiers.cpp's ModifySpellDamageTaken hook. Do NOT re-apply
            // it here or the bonus would stack twice on the same spell hit.

            // Improved Mutilate (5336) — Mutilate hits: chance to apply Rogue poison
            {
                // WotLK Mutilate spell IDs (base spell + ranks)
                static const std::unordered_set<uint32> s_mutilate = {
                    1329, 34411, 34412, 34413, 48661, 48662
                };
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_IMP_MUTILATE);
                if (rank > 0 && s_mutilate.count(spellInfo->Id) && target)
                {
                    static const float chance[] = { 0.0f, 10.0f, 20.0f, 30.0f };
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                    {
                        uint32 weaponCount = player->haveOffhandWeapon() ? 2u : 1u;
                        QueueRoguePoison(player, target, weaponCount);
                    }
                }
            }

            // Poison Master (5339) — finishers always apply Rogue poison
            // R1+R2: finisher always queues a poison stack. R3 raises max cap (handled in QueueRoguePoison).
            // R2 "crits always apply" is approximated as the same always-on R1 behavior (no crit-flag
            // hook available in 3.3.5a damage pipeline). Document approximation clearly.
            {
                static const std::unordered_set<uint32> s_finishers2 = {
                    2098, 6760, 6761, 6762, 8623, 8624, 11299, 11300, 26865, 48667, 48668,
                    1943, 8639, 8640, 11273, 11274, 11275, 26867, 48671, 48672,
                    408, 8643, 32645, 32684, 41487, 41488, 5171, 6774,
                    8647, 8649, 8650, 11197, 11198, 26866, 48669
                };
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_POISON_MASTER);
                if (rank > 0 && s_finishers2.count(spellInfo->Id) && target)
                {
                    // R2 "crits always apply poison" is APPROXIMATED as always-on (same as R1)
                    // because the crit flag is not accessible in ModifySpellDamageTaken.
                    uint32 weaponCount = player->haveOffhandWeapon() ? 2u : 1u;
                    QueueRoguePoison(player, target, weaponCount);
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

            // Scourge Mastery (Death Knight) — +15/25/35% Scourge Strike damage (per-rank scaling)
            // R2/R3 rider (queue disease on hit) is implemented via g_virulentPlague pattern below.
            {
                static const std::unordered_set<uint32> s_scourge = {
                    55090,55265,55270,55271
                };
                uint8 rank = SanctumAA::GetRank(player, AA_DK_SCOURGE_MASTERY);
                if (rank > 0 && s_scourge.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.35f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);

                    // R2+: queue applying BOTH Frost Fever (55095) + Blood Plague (55078) on next safe tick
                    // Uses a dedicated queue separate from Ghoul Infestation's alternating queue.
                    if (rank >= 2 && target)
                    {
                        uint32 guid = player->GetGUID().GetCounter();
                        g_scourgeDiseasesQueue[guid] = target->GetGUID().GetCounter();
                    }
                }
            }

            // Arctic Howl (Death Knight, 5515) — Howling Blast AoE-spike capstone.
            //   Damage:  +20/35/50% per rank.
            //   Guaranteed crit: Howling Blast always deals crit-magnitude damage.
            //     Implemented as a +50% multiplier (the WotLK spell-crit bonus) because this
            //     hook fires AFTER the crit roll — the number is crit-sized even if it does
            //     not always render yellow. (Design ruling: "always crit = guaranteed crit damage".)
            //   Disease spread (R2+): the FULL disease kit (Frost Fever + Blood Plague) is
            //     spread to every target Howling Blast hits. R1 relies on Howling Blast's
            //     native Frost Fever application. The hook fires once per AoE target, so each
            //     target is pushed into a multi-victim spread queue drained in OnUnitUpdate
            //     (CastSpell is unsafe here — re-entrancy). R3's extra +10yd spread is
            //     approximated by Howling Blast's native AoE radius covering the pack.
            {
                static const std::unordered_set<uint32> s_howlingBlast = {
                    49184,51411,51412
                };
                uint8 rank = SanctumAA::GetRank(player, AA_DK_ARCTIC_HOWL);
                if (rank > 0 && s_howlingBlast.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                    // Damage boost
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    // Guaranteed crit-magnitude (+50%, WotLK spell crit multiplier)
                    damage += (int32)(damage * 0.5f);

                    // R2+: queue full disease spread (FF + BP) onto this target.
                    if (rank >= 2 && target)
                    {
                        uint32 guid = player->GetGUID().GetCounter();
                        g_howlSpreadQueue[guid].push_back(target->GetGUID().GetCounter());
                    }
                }
            }

            // Improved Harm Touch (AA_DK_IMPROVED_HARM_TOUCH, 5526) —
            // Reframed: +15/30/45% Death Coil damage.
            // Catalog described "resist reduction" which is not cleanly moddable in 3.3.5a;
            // a flat damage bonus achieves the same power-increase intent.
            {
                static const std::unordered_set<uint32> s_deathCoil = {
                    47541, 47632, 47633, 49892, 49894, 49895, 52375, 59134
                };
                uint8 rank = SanctumAA::GetRank(player, AA_DK_IMPROVED_HARM_TOUCH);
                if (rank > 0 && s_deathCoil.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.15f, 0.30f, 0.45f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Rune Blade Mastery (5514) — PARTIAL: while Dancing Rune Weapon aura is active,
            // +5/10/15% spell damage done. DRW duration/cost/count mods are not moddable in 3.3.5a.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_RUNE_BLADE);
                if (rank > 0 && player->HasAura(49028u))
                {
                    static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Improved Drains (5802) — +10/18/30% Unstable Affliction damage
            {
                static const std::unordered_set<uint32> s_ua = {
                    30108, 30404, 30405, 47843, 47842
                };
                uint8 rank = SanctumAA::GetRank(player, AA_WRL_IMPROVED_DRAINS);
                if (rank > 0 && s_ua.count(spellInfo->Id))
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.18f, 0.30f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // Improved Devastate (5017) — +25% Devastate damage; apply stacking damage debuff on target
            {
                static const std::unordered_set<uint32> s_devastate = { 20243, 30016, 30022 };
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_IMPROVED_DEVASTATE);
                if (rank > 0 && s_devastate.count(spellInfo->Id) && target)
                {
                    // +25% Devastate damage (all ranks)
                    damage += (int32)(damage * 0.25f);

                    // Apply stacking debuff (logic lives in aa_combat_modifiers.cpp)
                    extern void SanctumAA_ApplyDevastateStack(uint32 attackerGuid, uint32 victimGuid, uint8 rank);
                    uint32 attackerGuid = player->GetGUID().GetCounter();
                    SanctumAA_ApplyDevastateStack(attackerGuid, target->GetGUID().GetCounter(), rank);
                }
            }

            // ── Elemental Overload (5616, renamed from Maelstrom Mastery) ──────────
            // Lightning Bolt / Chain Lightning / Lava Burst 25/35/45% chance → free second hit at 50% dmg.
            // SanctumAA_DealVisibleDamage is SAFE here (not a re-entrant recast — pure damage delivery).
            {
                static const std::unordered_set<uint32> s_lb = {
                    403,529,548,915,943,6041,10391,10392,15207,15208,25448,25449,49237,49238
                };
                static const std::unordered_set<uint32> s_cl = {
                    421,930,2860,10605,25439,25442,49268,49269
                };
                static const std::unordered_set<uint32> s_lavaburst = { 51505,60043 };

                uint8 rank = SanctumAA::GetRank(player, AA_SHA_ELEMENTAL_OVERLOAD);
                if (rank > 0 && target)
                {
                    bool isLBorCL   = s_lb.count(spellInfo->Id) || s_cl.count(spellInfo->Id);
                    bool isLavaBurst = s_lavaburst.count(spellInfo->Id) > 0;
                    if (isLBorCL || isLavaBurst)
                    {
                        static const float chance[] = { 0.0f, 25.0f, 35.0f, 45.0f };
                        if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        {
                            uint32 echoDmg = (uint32)(damage * 0.5f);
                            if (echoDmg > 0)
                            {
                                uint32 school = isLavaBurst ? SPELL_SCHOOL_MASK_FIRE
                                                            : SPELL_SCHOOL_MASK_NATURE;
                                SanctumAA_DealVisibleDamage(player, target, echoDmg, school);
                            }
                        }
                    }
                }
            }

            // ── Lava Surge (5613) — Lava Lash cleave splash ──────────────────────
            // Cleaves all enemies within rank-scaled radius. Safe: DealVisibleDamage only.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_SHA_LAVA_SURGE);
                if (rank > 0 && spellInfo->Id == 60103u && target)
                {
                    static const float radius[] = { 0.0f, 6.0f, 8.0f, 10.0f };
                    float r = radius[Idx<uint8>(rank)];
                    uint32 splashDmg = (uint32)(damage);
                    std::vector<Unit*> nearby;
                    for (Unit* atk : player->getAttackers())
                    {
                        if (atk != target && atk->IsAlive() && target->GetDistance(atk) <= r)
                            nearby.push_back(atk);
                    }
                    // Also sweep current target's area in case not in attacker list
                    // (attackers list is the reliable source; supplemental GetVictim not needed)
                    for (Unit* u : nearby)
                        SanctumAA_DealVisibleDamage(player, u, splashDmg, SPELL_SCHOOL_MASK_FIRE);
                }
            }

            // ── Scorched Earth (5614) — Lava Burst: +10% dmg + queue Fire DoT ────
            // +10% flat Lava Burst damage at all ranks.
            // DoT = 20/35/50% of hit over 6s (2s ticks = 3 ticks). Queued for safety.
            {
                static const std::unordered_set<uint32> s_lburst = { 51505,60043 };
                uint8 rank = SanctumAA::GetRank(player, AA_SHA_SCORCHED_EARTH);
                if (rank > 0 && s_lburst.count(spellInfo->Id) && target)
                {
                    // +10% LB damage (all ranks)
                    damage += (int32)(damage * 0.10f);

                    // Queue DoT: 20/35/50% of (original) hit over 6s, 3 ticks
                    static const float dotPct[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                    uint32 totalDot = (uint32)(damage * dotPct[Idx<uint8>(rank)]);
                    uint32 tickDmg  = std::max(1u, totalDot / 3u);
                    uint32 victLow  = target->GetGUID().GetCounter();
                    uint32 guid2    = player->GetGUID().GetCounter();
                    // Store tick damage in queue for safe application on next OnUnitUpdate
                    // Encode as a special entry: we push into g_scorchedQueue and store tickDmg
                    // separately in g_scorchedEarth directly (safe outside damage hooks at setup time)
                    auto& se = g_scorchedEarth[guid2][victLow];
                    se.tickDmg   = tickDmg;
                    se.endMs     = getMSTime() + 6000u;
                    se.lastTickMs = getMSTime();
                }
            }

            // ── Lightning Rod (5615) — Chain Lightning creates a 6s bounce window ─
            // PARTIAL: after CL hits, open a window that re-fires CL at 30/50/70% every 2s.
            // The rod fires from the player targeting the same victim for up to 3 bounces (6s).
            // Implemented as a throttled window in OnUnitUpdate; no rod creature spawned.
            {
                static const std::unordered_set<uint32> s_cl2 = {
                    421,930,2860,10605,25439,25442,49268,49269
                };
                uint8 rank = SanctumAA::GetRank(player, AA_SHA_LIGHTNING_ROD);
                if (rank > 0 && s_cl2.count(spellInfo->Id) && target)
                {
                    // Open a 6s Lightning Rod window on this target.
                    // Echo damage is computed in OnUnitUpdate from current SP (same as Spirit Lash pattern).
                    // 30/50/70% of SP as Nature, re-fired every 2s for 6s (3 ticks).
                    uint32 guid2 = player->GetGUID().GetCounter();
                    auto& rod = g_lightningRod[guid2];
                    rod.expireMs   = getMSTime() + 6000u;
                    rod.lastFireMs = getMSTime();
                    rod.targetLow  = target->GetGUID().GetCounter();
                    rod.rank       = rank;
                }
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
            // Slippery (5320) — AoE damage taken -8/15/25%
            {
                uint8 rank = SanctumAA::GetRank(player, AA_ROG_SLIPPERY);
                if (rank > 0 && spellInfo->IsAffectingArea())
                {
                    static const float dr[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                    damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                }
            }

            // Burning Soul (Warlock, 5809) — 20/35/50% incoming damage drained from mana instead.
            // Stops working when the player has no mana left.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WRL_BURNING_SOUL);
                if (rank > 0)
                {
                    uint32 manaAvail = player->GetPower(POWER_MANA);
                    if (manaAvail > 0)
                    {
                        static const float pct[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                        uint32 absorb = std::min(static_cast<uint32>(damage * pct[Idx<uint8>(rank)]), manaAvail);
                        damage -= (int32)absorb;
                        player->ModifyPower(POWER_MANA, -(int32)absorb);
                    }
                }
            }

            // Umbral Leech (Warlock, 5808) — Hellfire self-damage heals player for 1/2/3% HP per tick.
            // Hellfire ticks deal damage to the caster through the same aura; we detect this by
            // target == attacker and a Hellfire spell ID.
            {
                static const std::unordered_set<uint32> s_hellfire = {
                    1949, 11682, 11683, 27212, 47897, 47898
                };
                uint8 rank = SanctumAA::GetRank(player, AA_WRL_UMBRAL_LEECH);
                if (rank > 0 && attacker == target && spellInfo && s_hellfire.count(spellInfo->Id))
                {
                    static const float pct[] = { 0.0f, 0.01f, 0.02f, 0.03f };
                    int32 healAmt = (int32)(player->GetMaxHealth() * pct[Idx<uint8>(rank)]);
                    if (healAmt > 0 && !player->IsFullHealth())
                        player->ModifyHealth(healAmt);
                }
            }

            // Unholy Guard (DK, 5505) — absorbs 5/8/12% of incoming spell damage, spending Runic Power.
            // NOTE: secondary/tertiary DKs get RP topped up via a hidden pool in mod-multiclass, so
            // checking GetPower(POWER_RUNIC_POWER) works regardless of the character's primary power type.
            // 1 Runic Power absorbs approximately 1 point of damage (RP pool caps the absorb amount).
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_UNHOLY_GUARD);
                if (rank > 0 && damage > 0)
                {
                    uint32 rpAvail = player->GetPower(POWER_RUNIC_POWER);
                    if (rpAvail > 0)
                    {
                        static const float pct[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                        int32 absorb = std::min(static_cast<int32>(damage * pct[Idx<uint8>(rank)]),
                                                static_cast<int32>(rpAvail));
                        if (absorb > 0)
                        {
                            damage -= absorb;
                            player->ModifyPower(POWER_RUNIC_POWER, -absorb);
                        }
                    }
                }
            }

            // Iron Shell (5507) — PARTIAL: while AMS (48707) or Bone Shield (49222) is active,
            // apply -10/15/20% DR on incoming spell damage.
            // "+25% AMS absorb cap" from catalog is approximated by this flat DR.
            // R3 CD reduction on cast is handled in OnPlayerSpellCast.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_IRON_SHELL);
                if (rank > 0 && damage > 0 &&
                    (player->HasAura(48707u) || player->HasAura(49222u)))
                {
                    static const float dr[] = { 0.0f, 0.10f, 0.15f, 0.20f };
                    damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                }
            }

            // Ancestral Guard (5605) — consume absorb shield on incoming spell damage
            {
                uint32 vGuid2 = player->GetGUID().GetCounter();
                auto it = g_ancestralGuardAbsorb.find(vGuid2);
                if (it != g_ancestralGuardAbsorb.end() && it->second.absorb > 0 && damage > 0)
                {
                    if (getMSTime() > it->second.expireMs)
                    {
                        it->second.absorb = 0;
                    }
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
    // ModifyPeriodicDamageAurasTick
    // -----------------------------------------------------------------------
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
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

        // Soul Abrasion (5830) — Drain Life ticks: bonus healing to caster (+15/25/40% of tick dmg)
        // Drain Life normally heals the caster for the damage dealt; this adds an extra fraction.
        // Implemented here (ModifyPeriodicDamageAurasTick) so we can call ModifyHealth.
        // This is safe because we are NOT inside ModifyMeleeDamage/ModifySpellDamageTaken.
        {
            static const std::unordered_set<uint32> s_drainLife = {
                689, 699, 709, 7651, 11699, 11700, 27219, 27220, 47857
            };
            uint8 rank = SanctumAA::GetRank(player, AA_WRL_SOUL_ABRASION);
            if (rank > 0 && s_drainLife.count(spellInfo->Id) && !player->IsFullHealth())
            {
                static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                int32 extraHeal = (int32)(damage * bonus[Idx<uint8>(rank)]);
                if (extraHeal > 0)
                    player->ModifyHealth(extraHeal);
            }
        }

        // Improved Curses (5806) — +15/25/40% Curse of Agony damage
        {
            static const std::unordered_set<uint32> s_coa = {
                980, 1014, 6217, 11711, 11712, 11713, 27218, 47863, 47864
            };
            uint8 rank = SanctumAA::GetRank(player, AA_WRL_IMPROVED_CURSES);
            if (rank > 0 && s_coa.count(spellInfo->Id))
            {
                static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
            }
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

        // Improved Rupture (5330) — Rupture ticks: 15/25/40% chance to apply Rogue poison
        {
            static const std::unordered_set<uint32> s_ruptureDot = {
                1943, 8639, 8640, 11273, 11274, 11275, 26867, 48671, 48672
            };
            uint8 rank = SanctumAA::GetRank(player, AA_ROG_IMP_RUPTURE);
            if (rank > 0 && s_ruptureDot.count(spellInfo->Id) && target)
            {
                static const float chance[] = { 0.0f, 15.0f, 25.0f, 40.0f };
                if (roll_chance_f(chance[Idx<uint8>(rank)]))
                {
                    uint32 weaponCount = player->haveOffhandWeapon() ? 2u : 1u;
                    QueueRoguePoison(player, target, weaponCount);
                }
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

        // Poison Arrow DoT ticking (Hunter) — nature, 1 tick per 2s, 5 ticks total
        {
            auto paIt = g_poisonArrow.find(guid);
            if (paIt != g_poisonArrow.end() && !paIt->second.empty())
            {
                std::vector<uint32> toErase;
                for (auto& [victLow, pstate] : paIt->second)
                {
                    if (now > pstate.endMs) { toErase.push_back(victLow); continue; }
                    if (GetMSTimeDiffToNow(pstate.lastTickMs) < 2000u) continue;
                    pstate.lastTickMs = now;
                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                    if (!victim)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                    }
                    if (!victim || !victim->IsAlive()) continue;
                    Unit::DealDamage(player, victim, pstate.tickDmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NATURE, nullptr, false);
                }
                for (uint32 v : toErase) paIt->second.erase(v);
            }
        }

        // Burning Arrow DoT ticking (Hunter) — fire, 1 tick per 2s, 3 ticks total
        {
            auto baIt = g_burningArrow.find(guid);
            if (baIt != g_burningArrow.end() && !baIt->second.empty())
            {
                std::vector<uint32> toErase;
                for (auto& [victLow, bstate] : baIt->second)
                {
                    if (now > bstate.endMs) { toErase.push_back(victLow); continue; }
                    if (GetMSTimeDiffToNow(bstate.lastTickMs) < 2000u) continue;
                    bstate.lastTickMs = now;
                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                    if (!victim)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                    }
                    if (!victim || !victim->IsAlive()) continue;
                    Unit::DealDamage(player, victim, bstate.tickDmg, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_FIRE, nullptr, false);
                }
                for (uint32 v : toErase) baIt->second.erase(v);
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

        // Rogue Poison DoT ticking — 1 tick per 2s, nature school (visible damage)
        {
            auto rpIt = g_roguePoison.find(guid);
            if (rpIt != g_roguePoison.end() && !rpIt->second.empty())
            {
                std::vector<uint32> toErase;
                for (auto& [victLow, rpState] : rpIt->second)
                {
                    if (now > rpState.endMs) { toErase.push_back(victLow); continue; }
                    if (GetMSTimeDiffToNow(rpState.lastTickMs) < 2000u) continue;
                    rpState.lastTickMs = now;

                    // Locate victim
                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                    if (!victim)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                    }
                    if (!victim || !victim->IsAlive()) continue;

                    // Deliver visible poison damage
                    SanctumAA_DealVisibleDamage(player, victim, rpState.tickDmg, SPELL_SCHOOL_MASK_NATURE);

                    // Leeching Toxins (5342) — heal player for a % of poison tick damage
                    {
                        uint8 lt = SanctumAA::GetRank(player, AA_ROG_LEECHING_TOXINS);
                        if (lt > 0 && !player->IsFullHealth())
                        {
                            static const float lf[] = { 0.0f, 0.04f, 0.07f, 0.10f };
                            int32 heal = (int32)(rpState.tickDmg * lf[Idx<uint8>(lt)]);
                            if (heal > 0)
                                player->ModifyHealth(heal);
                        }
                    }
                }
                for (uint32 v : toErase) rpIt->second.erase(v);
            }
        }

        // Invigoration (5338) — gain 10 Energy every 4/3/2s if a poison was applied that window
        {
            uint8 ir = SanctumAA::GetRank(player, AA_ROG_INVIGORATION);
            if (ir > 0 && player->getPowerType() == POWER_ENERGY)
            {
                static const uint32 win[] = { 0, 4000u, 3000u, 2000u };
                uint32 w = win[Idx<uint8>(ir)];
                auto& invTick = g_invigorTick[guid];
                if (GetMSTimeDiffToNow(invTick) >= w)
                {
                    invTick = now;
                    auto pit = g_poisonAppliedMs.find(guid);
                    if (pit != g_poisonAppliedMs.end() && GetMSTimeDiffToNow(pit->second) <= w)
                        player->ModifyPower(POWER_ENERGY, 10);
                }
            }
        }

        // ── Ghoul Infestation (5521) — drain queue: apply disease cast deferred from pet-melee hook ──
        // The actual queue entry is set in aa_pet.cpp's ModifyMeleeDamage via SanctumAA_QueueGhoulInfest.
        // It is safe to CastSpell here because OnUnitUpdate is outside all damage hooks.
        {
            auto qit = g_ghoulInfestQueue.find(guid);
            if (qit != g_ghoulInfestQueue.end())
            {
                uint32 victLow = qit->second;
                g_ghoulInfestQueue.erase(qit);

                // Locate victim by GUID counter
                Unit* victim = nullptr;
                for (Unit* atk : player->getAttackers())
                    if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                if (!victim)
                {
                    Unit* v = player->GetVictim();
                    if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                }
                if (victim && victim->IsAlive())
                {
                    bool& toggle = g_ghoulInfestToggle[guid];
                    // Apply Frost Fever (55095) or Blood Plague (55078), alternating each proc.
                    uint32 diseaseId = toggle ? 55078u : 55095u;
                    toggle = !toggle;
                    player->CastSpell(victim, diseaseId, true);
                }
            }
        }

        // ── Scourge Mastery R2+ disease application — drain queue ──────────────────
        // Queue is set in ModifySpellDamageTaken when a Scourge Strike hits.
        // Applies BOTH Frost Fever (55095) and Blood Plague (55078) at once.
        {
            auto sqit = g_scourgeDiseasesQueue.find(guid);
            if (sqit != g_scourgeDiseasesQueue.end())
            {
                uint32 victLow = sqit->second;
                g_scourgeDiseasesQueue.erase(sqit);

                Unit* victim = nullptr;
                for (Unit* atk : player->getAttackers())
                    if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                if (!victim)
                {
                    Unit* v = player->GetVictim();
                    if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                }
                if (victim && victim->IsAlive())
                {
                    player->CastSpell(victim, 55095u, true); // Frost Fever
                    player->CastSpell(victim, 55078u, true); // Blood Plague
                }
            }
        }

        // ── Arctic Howl R2+ (5515) — drain AoE disease-spread queue ────────────────
        // Queue is filled (one entry per Howling Blast target) in ModifySpellDamageTaken.
        // Casts BOTH diseases on every target hit. Safe here (outside damage hooks).
        {
            auto hqit = g_howlSpreadQueue.find(guid);
            if (hqit != g_howlSpreadQueue.end())
            {
                std::vector<uint32> victims;
                victims.swap(hqit->second);
                g_howlSpreadQueue.erase(hqit);

                for (uint32 victLow : victims)
                {
                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                    if (!victim)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                    }
                    if (victim && victim->IsAlive())
                    {
                        player->CastSpell(victim, 55095u, true); // Frost Fever
                        player->CastSpell(victim, 55078u, true); // Blood Plague
                    }
                }
            }
        }

        // ── Virulent Plague (5520) — ticking Nature DoT (2s ticks, 8s duration) ──
        // Queue is set in OnPlayerSpellCast when Plague Strike IDs are detected.
        // Drain the queue here first, then tick any active DoTs.
        {
            // Drain pending application
            auto vqit = g_virulentQueue.find(guid);
            if (vqit != g_virulentQueue.end())
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_VIRULENT_PLAGUE);
                if (rank > 0)
                {
                    uint32 victLow = vqit->second;
                    auto& vpState  = g_virulentPlague[guid][victLow];
                    uint32 ap      = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
                    // Tick damage: AP * 6% per rank (R1=6%, R2=10%, R3=15%)
                    static const float tickPct[] = { 0.0f, 0.06f, 0.10f, 0.15f };
                    vpState.tickDmg   = std::max(1u, (uint32)(ap * tickPct[Idx<uint8>(rank)]));
                    vpState.endMs     = now + 8000u;
                    vpState.lastTickMs = now;
                }
                g_virulentQueue.erase(vqit);
            }

            // Tick active DoTs
            auto vpIt = g_virulentPlague.find(guid);
            if (vpIt != g_virulentPlague.end() && !vpIt->second.empty())
            {
                std::vector<uint32> toErase;
                for (auto& [victLow, vpState] : vpIt->second)
                {
                    if (now > vpState.endMs) { toErase.push_back(victLow); continue; }
                    if (GetMSTimeDiffToNow(vpState.lastTickMs) < 2000u) continue;
                    vpState.lastTickMs = now;

                    // Locate victim
                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                    if (!victim)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                    }
                    if (!victim || !victim->IsAlive()) continue;

                    SanctumAA_DealVisibleDamage(player, victim, vpState.tickDmg, SPELL_SCHOOL_MASK_NATURE);
                }
                for (uint32 v : toErase) vpIt->second.erase(v);
            }
        }

        // ── Scorched Earth (5614) — ticking Fire DoT (3 ticks, 2s each, 6s total) ──
        // Application happens directly in ModifySpellDamageTaken (safe for struct writes).
        // Ticking drained here (SanctumAA_DealVisibleDamage is always safe in OnUnitUpdate).
        {
            auto seIt = g_scorchedEarth.find(guid);
            if (seIt != g_scorchedEarth.end() && !seIt->second.empty())
            {
                std::vector<uint32> toErase;
                for (auto& [victLow, seState] : seIt->second)
                {
                    if (now > seState.endMs) { toErase.push_back(victLow); continue; }
                    if (GetMSTimeDiffToNow(seState.lastTickMs) < 2000u) continue;
                    seState.lastTickMs = now;

                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == victLow) { victim = atk; break; }
                    if (!victim)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == victLow) victim = v;
                    }
                    if (!victim || !victim->IsAlive()) continue;
                    SanctumAA_DealVisibleDamage(player, victim, seState.tickDmg, SPELL_SCHOOL_MASK_FIRE);
                }
                for (uint32 v : toErase) seIt->second.erase(v);
            }
        }

        // ── Ghost Strike (5617) — every 3/2/1.5s: strike random nearby enemy 60% weapon dmg as Nature ──
        // Weapon damage approximated as AP/14.0 * 1.5 (normalized for 1.5s swing) × 0.60.
        // The interval varies per rank: R1=3000ms, R2=2000ms, R3=1500ms.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_SHA_GHOST_STRIKE);
            if (rank > 0)
            {
                static const uint32 interval[] = { 0u, 3000u, 2000u, 1500u };
                uint32 iv = interval[Idx<uint8>(rank)];
                auto& stamp = g_ghostStrikeTick[guid];
                if (GetMSTimeDiffToNow(stamp) >= iv)
                {
                    stamp = now;
                    // Find a nearby enemy within 8 yards
                    Unit* target2 = nullptr;
                    for (Unit* atk : player->getAttackers())
                    {
                        if (atk->IsAlive() && player->GetDistance(atk) <= 8.0f)
                        {
                            target2 = atk;
                            break;
                        }
                    }
                    if (!target2)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->IsAlive() && player->GetDistance(v) <= 8.0f)
                            target2 = v;
                    }
                    if (target2)
                    {
                        // Weapon dmg approx: AP/14 * 1.5 * 0.60 (60% of one normalized swing)
                        uint32 ap = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
                        uint32 strikeDmg = std::max(1u, (uint32)(ap / 14.0f * 1.5f * 0.60f));
                        SanctumAA_DealVisibleDamage(player, target2, strikeDmg, SPELL_SCHOOL_MASK_NATURE);
                    }
                }
            }
        }

        // ── Lightning Rod (5615) — re-fires CL at 30/50/70% SP every 2s for 6s ─
        // PARTIAL: re-fires from player targeting the stored victim at SP-based damage.
        // No rod creature; implemented as throttled OnUnitUpdate damage bursts.
        {
            auto rodIt = g_lightningRod.find(guid);
            if (rodIt != g_lightningRod.end() && rodIt->second.expireMs > 0)
            {
                auto& rod = rodIt->second;
                if (now > rod.expireMs)
                {
                    rod.expireMs = 0;
                }
                else if (GetMSTimeDiffToNow(rod.lastFireMs) >= 2000u)
                {
                    rod.lastFireMs = now;
                    // Locate target
                    Unit* rodTarget = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == rod.targetLow) { rodTarget = atk; break; }
                    if (!rodTarget)
                    {
                        Unit* v = player->GetVictim();
                        if (v && v->GetGUID().GetCounter() == rod.targetLow) rodTarget = v;
                    }
                    if (rodTarget && rodTarget->IsAlive())
                    {
                        static const float pct[] = { 0.0f, 0.30f, 0.50f, 0.70f };
                        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE);
                        uint32 rodDmg = std::max(1u, (uint32)(sp * pct[Idx<uint8>(rod.rank)]));
                        SanctumAA_DealVisibleDamage(player, rodTarget, rodDmg, SPELL_SCHOOL_MASK_NATURE);
                    }
                }
            }
        }

        // ── Earth Shield queue drain (Ancestral Bulwark 5619) ────────────────────
        // Queue is filled in aa_combat_modifiers.cpp ModifyHealReceived (on Chain Heal hits).
        // CastSpell is safe here (OnUnitUpdate, outside all damage hooks).
        {
            auto esIt = g_earthShieldQueue.find(guid);
            if (esIt != g_earthShieldQueue.end() && !esIt->second.empty())
            {
                std::vector<uint32> targets;
                targets.swap(esIt->second);
                g_earthShieldQueue.erase(esIt);

                uint8 rank = SanctumAA::GetRank(player, AA_SHA_ANCESTRAL_BULWARK);
                // Per-rank Earth Shield potency: R1→32593, R2→49283, R3→49284
                static const uint32 esId[] = { 0u, 32593u, 49283u, 49284u };
                uint32 shieldSpell = (rank > 0) ? esId[Idx<uint8>(rank)] : 0u;
                if (shieldSpell > 0u)
                {
                    for (uint32 tLow : targets)
                    {
                        // Try to find the target unit: self, real pet slot, or guardian pet
                        Unit* esTarget = nullptr;
                        if (player->GetGUID().GetCounter() == tLow)
                        {
                            esTarget = player;
                        }
                        else
                        {
                            Pet* pet = player->GetPet();
                            if (pet && pet->GetGUID().GetCounter() == tLow) esTarget = pet;
                            if (!esTarget)
                            {
                                Unit* guardian = player->GetGuardianPet();
                                if (guardian && guardian->GetGUID().GetCounter() == tLow) esTarget = guardian;
                            }
                        }
                        if (esTarget && esTarget->IsAlive())
                            player->CastSpell(esTarget, shieldSpell, true);
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

        // ── Totemic Mastery (5604) — PARTIAL STUB ────────────────────────────
        // Effect: all totem durations +30/60/120s.
        // 3.3.5a does not expose a "totem despawn timer" through the modding API.
        // Totems are Creature* TempSummons summoned with a fixed duration from the spell's
        // EFFECT_SUMMON handler; there is no clean post-spawn method to extend their lifetime
        // without casting them again. Proper implementation would require an OnSummon hook
        // or a hardcoded creature-script — neither is available without core modifications.
        // STUB: effect documented here, no implementation. In-game: the AA slot is purchaseable
        // but has no runtime effect until a proper hook is available.
        // TODO: revisit if AzerothCore adds an OnTotemSummon / TempSummon duration hook.
        (void)SanctumAA::GetRank(player, AA_SHA_TOTEMIC_MASTERY); // suppress unused-variable warning

        // ── Elemental Accord (5620) — per-5s passive per-active-totem buff ────
        // Each active totem type (Earth/Fire/Water/Air) gives a stacking AP bonus.
        // Bonus: +150 AP per active totem element at base; R2/R3: ×1.5/×2.0.
        // Totem detection: Shaman totems are TempSummons of specific creature families.
        // We approximate by counting the player's active guardian creatures that match
        // known totem entry IDs, which avoids a full creature-type scan.
        // APPROXIMATION: detects active guardian count (any guardian = "totem active");
        // full element-keyed detection would require creature-family checks per entry.
        // Bonus recalculated each 5s tick; old bonus removed before new one applied.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_SHA_ELEMENTAL_ACCORD);
            if (rank > 0)
            {
                static const float multiplier[] = { 0.0f, 1.0f, 1.5f, 2.0f };
                float mult = multiplier[Idx<uint8>(rank)];

                // Count active guardians (proxy for active totems; max 4 = 4 elements)
                uint8 activeCount = 0;
                Unit* guardian = player->GetGuardianPet();
                if (guardian && guardian->IsAlive()) activeCount++;
                // Also check the m_Controlled list if accessible via getAttackers (indirect)
                // Guardians in m_Controlled are the best source but no public API to iterate them.
                // Clamp to 4 (4 totem slots max)
                if (activeCount > 4) activeCount = 4;

                // Flat AP per totem: 150 at R1 base
                int32 newAP = (int32)(150.0f * activeCount * mult);

                auto& acc = g_elementalAccord[guid];
                // Remove old applied bonus, apply new
                if (acc.appliedAP != 0)
                {
                    player->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE,
                                                   (float)acc.appliedAP, false);
                    player->UpdateAttackPowerAndDamage(false);
                }
                if (newAP != 0)
                {
                    player->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE,
                                                   (float)newAP, true);
                    player->UpdateAttackPowerAndDamage(false);
                }
                acc.appliedAP = newAP;
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
        Player* p = unit->ToPlayer();
        uint32 guid = p->GetGUID().GetCounter();

        // Elemental Accord: remove any lingering AP bonus before clearing state
        {
            auto it = g_elementalAccord.find(guid);
            if (it != g_elementalAccord.end() && it->second.appliedAP != 0)
            {
                p->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER, TOTAL_VALUE,
                                          (float)it->second.appliedAP, false);
                p->UpdateAttackPowerAndDamage(false);
                it->second.appliedAP = 0;
            }
        }
        g_elementalAccord.erase(guid);

        ClearPlayerState(guid, p);
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
    // OnPlayerSpellCast — Paladin cast-based AAs.
    // -----------------------------------------------------------------------
    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override
    {
        if (!player || !spell || skipCheck)
            return;

        SpellInfo const* info = spell->GetSpellInfo();
        if (!info)
            return;

        uint32 guid = player->GetGUID().GetCounter();

        // Judge (5101) — detect Judgement cast, open Judge window
        {
            // Judgement of Light / Wisdom / Justice (the base judgement apply spells)
            static const std::unordered_set<uint32> s_judgement = {
                20271, 53408, 53407,    // Judgement of Light / Wisdom / Justice
                20184, 20186, 20187,    // Judgement (older ranks)
                54158                   // Judgement
            };
            uint8 rank = SanctumAA::GetRank(player, AA_PAL_JUDGE);
            if (rank > 0 && s_judgement.count(info->Id))
            {
                // Open Judge window: R1=3, R2=4, R3=5 swings remaining. Window lasts 15s.
                static const uint8 swings[] = { 0, 3, 4, 5 };
                extern void SanctumAA_OpenJudgeWindow(uint32 guid, uint8 swings, uint32 durationMs);
                SanctumAA_OpenJudgeWindow(guid, swings[Idx<uint8>(rank)], 15000u);
            }
        }

        // Purifying Judgment (5125) — 25/50/75% chance on Judgement to dispel one magic/disease
        {
            static const std::unordered_set<uint32> s_judgement2 = {
                20271, 53408, 53407, 20184, 20186, 20187, 54158
            };
            uint8 rank = SanctumAA::GetRank(player, AA_PAL_PURIFYING_JUDGMENT);
            if (rank > 0 && s_judgement2.count(info->Id))
            {
                static const float chance[] = { 0.0f, 25.0f, 50.0f, 75.0f };
                if (roll_chance_f(chance[Idx<uint8>(rank)]))
                {
                    // Remove one harmful magic or disease aura from the player
                    for (auto& auraMap : player->GetAppliedAuras())
                    {
                        AuraApplication const* aurApp = auraMap.second;
                        if (!aurApp) continue;
                        SpellInfo const* auraInfo = aurApp->GetBase()->GetSpellInfo();
                        if (!auraInfo) continue;
                        if (aurApp->IsPositive()) continue; // skip beneficial auras
                        uint32 dispel = auraInfo->Dispel;
                        if (dispel == DISPEL_MAGIC || dispel == DISPEL_DISEASE)
                        {
                            player->RemoveAura(auraMap.first);
                            break; // only one
                        }
                    }
                }
            }
        }

        // Improved Flash of Light (5114) — each Flash of Light cast adds a Radiance stack (max 5)
        {
            static const std::unordered_set<uint32> s_fol = {
                19750, 19939, 19940, 19941, 19942, 25363, 27137, 48784, 48785
            };
            uint8 rank = SanctumAA::GetRank(player, AA_PAL_IMPROVED_FLASH_OF_LIGHT);
            if (rank > 0 && s_fol.count(info->Id))
            {
                extern void SanctumAA_AddRadianceStack(uint32 guid);
                SanctumAA_AddRadianceStack(guid);
            }
        }

        // Lay of Hands Mastery (5117) — reduce LoH cooldown on cast via SpellHistory
        {
            static const std::unordered_set<uint32> s_loh = { 633, 2800, 10310, 27154, 48788 };
            uint8 rank = SanctumAA::GetRank(player, AA_PAL_LAY_OF_HANDS_MASTERY);
            if (rank > 0 && s_loh.count(info->Id))
            {
                // Default LoH base CD = 20 min (1200s). Reduce per rank: -20/-40/-60 min.
                // Since we're IN the cast handler, the CD gets set AFTER this fires in some engines.
                // We reduce it by modifying after a brief update — actually the simplest approach is
                // to call player->ModifySpellCooldown after cast. The hook fires during cast, not after.
                // Apply a CD override: reduce by 20/40/60 minutes in milliseconds.
                // Default LoH base CD = 20 min (1200s). Reduce per rank: -20/-40/-60 min.
                // ModifySpellCooldown adjusts the existing cooldown by the given ms delta.
                static const int32 cdRedMs[] = { 0, 1200000, 2400000, 3600000 };
                int32 reduction = cdRedMs[Idx<uint8>(rank)];
                for (uint32 spellId : s_loh)
                    player->ModifySpellCooldown(spellId, -reduction);
            }
        }

        // ── DK: Soul Abrasion (5524) — Death Strike self-heal +15/25/40% ─────────
        // Approach: when a Death Strike rank is cast, queue an additional flat heal
        // equal to 5/8/12% max HP on next OnUnitUpdate. (The actual Death Strike heal
        // fires after the cast resolves; ModifyHealReceived is the cleaner hook but
        // Death Strike's self-heal is a hardcoded unit-heal that does not flow through
        // UNITHOOK_MODIFY_HEAL_RECEIVED in 3.3.5a. Flat supplemental heal is cleanest.)
        // Flat supplemental: R1 5%, R2 8%, R3 12% max HP.
        {
            static const std::unordered_set<uint32> s_deathStrike = {
                49998, 49999, 50000, 45463, 49923, 49924, 66188
            };
            uint8 rank = SanctumAA::GetRank(player, AA_DK_SOUL_ABRASION);
            if (rank > 0 && s_deathStrike.count(info->Id) && !player->IsFullHealth())
            {
                static const float pct[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                int32 healAmt = (int32)(player->GetMaxHealth() * pct[Idx<uint8>(rank)]);
                if (healAmt > 0)
                    player->ModifyHealth(healAmt);

                // Bonus Runic Power on Death Strike cast: +10/15/20 RP
                static const int32 rpGain[] = { 0, 10, 15, 20 };
                int32 rp = rpGain[Idx<uint8>(rank)];
                if (rp > 0)
                {
                    int32 curRP  = (int32)player->GetPower(POWER_RUNIC_POWER);
                    int32 maxRP  = (int32)player->GetMaxPower(POWER_RUNIC_POWER);
                    int32 newRP  = std::min(curRP + rp, maxRP);
                    player->SetPower(POWER_RUNIC_POWER, (uint32)newRP);
                }
            }
        }

        // ── DK: Iron Shell (5507) — on cast of AMS (48707) or Bone Shield (49222): reduce their CD ──
        // PARTIAL: The "+25% AMS absorb cap" rider is approximated by the -10/15/20% DR-while-shielded
        // block in the damage hooks below. Here we only handle the CD reduction at R3.
        // R3: -20% to AMS and Bone Shield cooldowns on cast.
        {
            static const std::unordered_set<uint32> s_ironShellSpells = { 48707u, 49222u };
            if (SanctumAA::GetRank(player, AA_DK_IRON_SHELL) >= 3 && s_ironShellSpells.count(info->Id))
            {
                // AMS base CD = 45s (45000ms), Bone Shield = 60s (60000ms). Reduce by 20%.
                for (uint32 spellId : s_ironShellSpells)
                    player->ModifySpellCooldown(spellId, -(int32)(45000u * 0.20f));
            }
        }

        // ── DK: Rune Blade Mastery (5514) — no CD to modify on DRW cast; see damage hook.
        // No spell-cast action needed; the damage bonus while DRW is active lives in
        // ModifySpellDamageTaken attacker-is-player section (see below).

        // ── DK: Battle Frenzy (5516) — on Hysteria cast, reduce its CD by 10/20/30% ──
        // PARTIAL: "Infinite duration at R3" is NOT implementable (can't modify running aura duration cleanly).
        {
            uint8 rank = SanctumAA::GetRank(player, AA_DK_BATTLE_FRENZY);
            if (rank > 0 && info->Id == 49016u)
            {
                // Hysteria base CD = 3 min (180000ms). Reduce by 10/20/30%.
                static const float cdRedPct[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                int32 reduction = (int32)(180000u * cdRedPct[Idx<uint8>(rank)]);
                player->ModifySpellCooldown(49016u, -reduction);
            }
        }

        // ── DK: Deathchill Mastery (5517) — STUB ──────────────────────────────────
        // Deathchill is not a standard 3.3.5a spell and was not found in mod-dk-rework sources.
        // If a future custom Deathchill spell ID is assigned, gate the effect here with HasSpell(id).
        // CURRENTLY: no-op. Effect intentionally left empty with this comment.

        // ── Shaman: Thunderous Strike (5612) — on Stormstrike, PARTIAL: proc Nature bonus hit ─
        // Catalog: 15/30/50% chance to proc MH+OH weapon enchants on Stormstrike.
        // "Force-proccing the active weapon imbue" requires knowing which imbue is on each weapon —
        // this is not cleanly accessible via player->GetWeaponEnchantProcEvent in 3.3.5a.
        // APPROXIMATION: on Stormstrike cast, chance-deal flat Nature bonus damage = 40% AP.
        // MH+OH = 2 chances applied independently; proc rate 15/30/50%.
        {
            static const std::unordered_set<uint32> s_stormstrike = { 17364,32175,32176,51876 };
            uint8 rank = SanctumAA::GetRank(player, AA_SHA_THUNDEROUS_STRIKE);
            if (rank > 0 && s_stormstrike.count(info->Id))
            {
                Unit* ssTarget = spell->m_targets.GetUnitTarget();
                if (!ssTarget) ssTarget = player->GetVictim();
                if (ssTarget && ssTarget->IsAlive())
                {
                    static const float chance[] = { 0.0f, 15.0f, 30.0f, 50.0f };
                    uint32 ap = (uint32)player->GetTotalAttackPowerValue(BASE_ATTACK);
                    uint32 procDmg = std::max(1u, (uint32)(ap * 0.40f));
                    // MH proc
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        SanctumAA_DealVisibleDamage(player, ssTarget, procDmg, SPELL_SCHOOL_MASK_NATURE);
                    // OH proc (if offhand weapon equipped)
                    if (player->haveOffhandWeapon() && roll_chance_f(chance[Idx<uint8>(rank)]))
                        SanctumAA_DealVisibleDamage(player, ssTarget, procDmg, SPELL_SCHOOL_MASK_NATURE);
                }
            }
        }

        // ── Shaman: Shock Resonance (5608) — on Shock cast, 10/20/30% chance to reset that shock's CD ──
        {
            static const std::unordered_set<uint32> s_shocks = {
                // Earth Shock
                8042,8044,8045,8046,10412,10413,10414,25454,49230,49231,
                // Flame Shock
                8050,8052,8053,10447,10448,29228,25457,49233,49234,
                // Frost Shock
                8056,8058,10472,10473,25464,49235,49236
            };
            uint8 rank = SanctumAA::GetRank(player, AA_SHA_SHOCK_RESONANCE);
            if (rank > 0 && s_shocks.count(info->Id))
            {
                static const float chance[] = { 0.0f, 10.0f, 20.0f, 30.0f };
                if (roll_chance_f(chance[Idx<uint8>(rank)]))
                    player->RemoveSpellCooldown(info->Id, true);
            }
        }

        // ── Shaman: Alpha Pack (5610) — Feral Spirit cast: reduce its CD ────────
        // R1: -15s from CD on cast. R3: -30s total (-15s more).
        // Spirit wolf haste/crit inherit is applied in aa_pet.cpp at wolf spawn.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_SHA_ALPHA_PACK);
            if (rank > 0 && info->Id == 51533u) // Feral Spirit
            {
                static const int32 cdRedMs[] = { 0, 15000, 15000, 30000 };
                int32 reduction = cdRedMs[Idx<uint8>(rank)];
                player->ModifySpellCooldown(51533u, -reduction);
            }
        }

        // ── Shaman: Swift Current (5618) — STUB ──────────────────────────────────
        // Nature's Swiftness +1 charge / GCD reduction mechanics are not cleanly moddable in 3.3.5a.
        // HasSpell(16188) can gate the check. Effect intentionally stubbed:
        //   - GCD manipulation: no exposed hook in 3.3.5a.
        //   - Extra charge: NS is flagged USABLE_WHILE_DEAD/ONE_SHOT in the engine; no charge counter.
        // STUB: if the player casts NS (16188), we simply remove its CD again — approximates "+1 charge".
        {
            uint8 rank = SanctumAA::GetRank(player, AA_SHA_SWIFT_CURRENT);
            if (rank > 0 && info->Id == 16188u && player->HasSpell(16188u))
            {
                // R1: grant one free extra use by immediately resetting the cooldown after cast.
                // R2/R3 GCD reduction: NOT implementable — no GCD hook in 3.3.5a.
                // Note: ModifySpellCooldown(-1200000) removes the full 20-min CD.
                player->ModifySpellCooldown(16188u, -1200000);
            }
        }

        // ── DK: Virulent Plague (5520) — on Plague Strike, queue a Virulent Plague DoT ──
        // The actual DoT ticking happens in OnUnitUpdate via the g_virulentQueue → g_virulentPlague path.
        // Duration extension rider (+4s to all diseases) is skipped — aura duration not moddable cleanly.
        {
            static const std::unordered_set<uint32> s_plagueStrike = {
                45462, 49917, 49918, 49919, 49920
            };
            uint8 rank = SanctumAA::GetRank(player, AA_DK_VIRULENT_PLAGUE);
            if (rank > 0 && s_plagueStrike.count(info->Id))
            {
                Unit* target = spell->m_targets.GetUnitTarget();
                if (!target) target = player->GetVictim();
                if (target && target->IsAlive())
                    g_virulentQueue[guid] = target->GetGUID().GetCounter();
            }
        }
    }

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

        // Plague's End (5518) — ONE-SHOT. On kill: if creature had any disease cast by the player,
        // heal player 15% max HP and spread those diseases to up to 2 nearby enemies within 15 yd.
        // Fires independently of Blood Rite (5504) and Pestilence (5502) — both may also fire.
        {
            if (SanctumAA::Has(player, AA_DK_PLAGUES_END))
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
                    // (a) Heal player 15% max HP
                    int32 healAmt = (int32)(player->GetMaxHealth() * 0.15f);
                    if (healAmt > 0 && !player->IsFullHealth())
                        player->ModifyHealth(healAmt);

                    // (b) Spread to up to 2 nearby attackers within 15 yd
                    uint8 spread = 0;
                    for (Unit* atk : player->getAttackers())
                    {
                        if (spread >= 2) break;
                        if (atk == creature || player->GetDistance(atk) > 15.0f) continue;
                        for (uint32 diseaseId : diseaseIds)
                            player->CastSpell(atk, diseaseId, true);
                        ++spread;
                    }
                }
            }
        }

        // Spreading Misery (5428) — SW:P/VT/DP jumps to nearest enemy on kill.
        // R3 raises jump range by +5yd (10→15 yd).
        // Mirror of Pestilence but for Priest shadow DoTs (by spell ID, not DISPEL_DISEASE).
        {
            uint8 rank = SanctumAA::GetRank(player, AA_PRI_SPREADING_MISERY);
            if (rank > 0)
            {
                static const std::unordered_set<uint32> s_priestDots = {
                    // Shadow Word: Pain all ranks
                    589, 594, 970, 992, 2767, 10892, 10893, 25367, 48124, 48125,
                    // Vampiric Touch all ranks
                    34914, 34916, 34917, 48159, 48160,
                    // Devouring Plague all ranks
                    2944, 19276, 19277, 19278, 25467, 48300, 48301
                };
                float jumpRange = (rank >= 3) ? 15.0f : 10.0f;

                std::unordered_set<uint32> dotIds;
                for (auto const& pair : creature->GetAppliedAuras())
                {
                    AuraApplication const* app = pair.second;
                    if (app->GetBase()->GetCasterGUID() == player->GetGUID() &&
                        s_priestDots.count(pair.first))
                    {
                        dotIds.insert(pair.first);
                    }
                }
                if (!dotIds.empty())
                {
                    // Jump to one nearby enemy (like Pestilence R1)
                    for (Unit* atk : player->getAttackers())
                    {
                        if (atk == creature || player->GetDistance(atk) > jumpRange)
                            continue;
                        for (uint32 dotId : dotIds)
                            player->CastSpell(atk, dotId, true);
                        break; // jump to one target (consistent with Pestilence R1 analog)
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
// Druid: Healing Gift (5919) — +3/6/10% heal crit chance (Tree form only)
// Applied as a stat passive via ApplyAAStat in mod-aa-system.cpp.
// NO class hook needed — the rating mod is applied on buy/login via ApplyAAStat.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void AddSC_aa_class()
{
    new aa_class_unit();
    new aa_class_player();
}
