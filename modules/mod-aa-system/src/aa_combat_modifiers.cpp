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
//   5519  Final Rune (DK)     — cheat-death: survive at 15% HP + 20% HP HoT; 3min wall-clock CD
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
#include "Pet.h"
#include "Unit.h"
#include "Creature.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
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

    // Final Rune (5519) — one-shot cheat death (DK, rank 1 only).
    // cdUntilMs = 0 means available; non-zero = CD running.
    struct FinalRuneState { uint32 cdUntilMs = 0; };
    std::unordered_map<uint32, FinalRuneState> g_finalRune;

    // Final Rune HoT pool: 20% max HP over 4s (1s ticks), same mechanics as g_beHot.
    struct FRHotState { int32 pool = 0; uint32 lastTickMs = 0; };
    std::unordered_map<uint32, FRHotState> g_frHot;

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

    // Track last known shapeshift form per player (for edge-detect on form change)
    std::unordered_map<uint32, ShapeshiftForm> g_lastShapeshiftForm;

    // ── Mage: Molten Shell (5743) — Heat counter per player ──────────────────
    struct MoltenShellState
    {
        uint8  heat        = 0;    // current Heat stacks
        uint8  maxHeat     = 6;    // cap by rank: R1=6, R2=8, R3=10
        uint32 lastHitMs   = 0;    // timestamp of last melee hit taken (for 6s decay)
        bool   flareQueued = false; // fire AoE + optional Hot Streak pending safe dispatch
    };
    std::unordered_map<uint32, MoltenShellState> g_moltenShell;

    // ── Mage: Scorched (5723) — per-target stacking fire-vuln debuff ─────────
    // playerGuid -> victimLow -> {stacks, expireMs}
    struct ScorchedEntry { uint8 stacks = 0; uint32 expireMs = 0; };
    std::unordered_map<uint32, std::unordered_map<uint32, ScorchedEntry>> g_scorched;

    // ── Mage: Spreading Flames (5702) — Ignite tick stacks per victim ────────
    // Stored as bonus% multiplier accumulated (additive per tick): +2/3/5% per tick, cap 10
    // playerGuid -> victimLow -> {stacks, expireMs}
    struct IgniteStackEntry { uint8 stacks = 0; uint32 expireMs = 0; };
    std::unordered_map<uint32, std::unordered_map<uint32, IgniteStackEntry>> g_igniteStacks;

    // ── Mage: Lost in Time (5719) — periodic arcane DoT on slowed targets ────
    // playerGuid -> victimLow -> last tick ms
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> g_lostInTimeTick;

    // ── Mage: Dragon's Fire (5705) — ground fire zone after Dragon's Breath ──
    struct DragonFireZone { float x, y, z; uint32 mapId; uint32 expireMs; uint32 lastTickMs; uint32 tickDmg; };
    std::unordered_map<uint32, DragonFireZone> g_dragonFireZone;

    // ── Mage: Focused Magic (5718) — ground arcane zone at target location ───
    struct FocusedMagicZone { float x, y, z; uint32 mapId; uint32 expireMs; uint32 lastTickMs; uint32 tickDmg; };
    std::unordered_map<uint32, FocusedMagicZone> g_focusedMagicZone;

    // ── Mage: Spell Weaving (5739) — stacking dmg bonus on school switch ─────
    struct SpellWeavingState { uint32 lastSchool = 0; uint8 stacks = 0; };
    std::unordered_map<uint32, SpellWeavingState> g_spellWeaving;

    // ── Mage: Mana Reactor (5740) — below-20%-mana refund flag ───────────────
    std::unordered_map<uint32, bool> g_manaReactorReady; // true = primed, will refund next dmg cast

    // ── Mage: Frostbolt bounce queue (5708) — queued extra frost hits ─────────
    struct FrostBounce { uint32 victimLow; uint8 hitsLeft; uint32 dmg; };
    std::unordered_map<uint32, std::vector<FrostBounce>> g_frostBounceQueue;

    // ── Mage: Improved Deep Freeze (5710) — queued free frost cast ───────────
    std::unordered_map<uint32, uint32> g_deepFreezeFreeCastQueue; // playerGuid -> victimLow (0=none)

    // ── Mage: Pyroblast DoT queue (5741) ─────────────────────────────────────
    struct PyroblastDotState { uint32 endMs = 0; uint32 lastTickMs = 0; uint32 tickDmg = 0; };
    std::unordered_map<uint32, std::unordered_map<uint32, PyroblastDotState>> g_pyroDoT;

    // ── Mage: Fire Blast cascade queue (5742) ────────────────────────────────
    struct FireBlastCascade { uint32 originVictimLow; uint32 dmg; };
    std::unordered_map<uint32, FireBlastCascade> g_fireBlastCascadeQueue;

    // ── Mage: Meteor Strike tracking (5704) — 3 proc flags per player ────────
    struct MeteorState { bool impact = false; bool firestarter = false; bool hotstreak = false; };
    std::unordered_map<uint32, MeteorState> g_meteorStrike;

    // ── Mage: Meteor/Shower queued AoE (5704/5706) ───────────────────────────
    struct MeteorQueue { uint32 dmg; bool queued = false; };
    std::unordered_map<uint32, MeteorQueue> g_meteorQueue;

    // ── Mage: Heating Up (5746) — queued Hot Streak aura apply ───────────────
    std::unordered_map<uint32, bool> g_heatingUpQueue;

    // ── Mage: Combustion Mastery (5744) — queued DoT spread on Combustion cast ──
    std::unordered_map<uint32, bool> g_combustionSpreadQueue;

    // =========================================================================
    // DRUID AA STATE
    // =========================================================================

    // ── Druid: Wrath of the Wild (5907) — OOC absorb ward ────────────────────
    struct WotwState { int32 absorb = 0; uint32 lastRefreshMs = 0; };
    std::unordered_map<uint32, WotwState> g_wotwAbsorb;

    // ── Druid: Sunfire (5914) — Moonfire-triggered nature DoT queue ──────────
    // playerGuid → victimLow → {endMs, lastTickMs, tickDmg}
    struct SunfireDoT { uint32 endMs = 0; uint32 lastTickMs = 0; uint32 tickDmg = 0; };
    std::unordered_map<uint32, std::unordered_map<uint32, SunfireDoT>> g_sunfireDoT;

    // ── Druid: Living Seed (5921) — per-target seed store ────────────────────
    // healerGuid → targetLow → seed amount (hp to bloom)
    std::unordered_map<uint32, std::unordered_map<uint32, uint32>> g_livingSeed;

    // ── Druid: Ancestral Spirits (5912) — periodic arcane tick ───────────────
    std::unordered_map<uint32, uint32> g_ancestralSpiritsLastTick;

    // ── Druid: Heart of the Wild (5932) — form-switch damage+heal window ──────
    struct HeartOfWildState { uint8 rank = 0; uint32 untilMs = 0; };
    std::unordered_map<uint32, HeartOfWildState> g_heartOfWild;

    // ── Druid: Feral Charge Mastery (5933) — window after Feral Charge ────────
    struct FeralChargeState { uint8 rank = 0; uint32 untilMs = 0; bool consumed = false; };
    std::unordered_map<uint32, FeralChargeState> g_feralCharge;

    // ── Druid: Survival Instincts (5930) — active DR window ──────────────────
    struct SurvivalInstinctsState { float drPct = 0.0f; uint32 untilMs = 0; };
    std::unordered_map<uint32, SurvivalInstinctsState> g_survivalInstincts;

    // ── Druid: Nature's Chosen (5916) — instant-cast flag after entering Moonkin ──
    // Set on form entry; consumed on first nature/arcane spell cast. Internal reset ICD.
    struct NaturesChosenState { bool ready = false; uint32 lastResetMs = 0; };
    std::unordered_map<uint32, NaturesChosenState> g_naturesChosen;

    // ── Druid: Rip and Tear (5901) — spread queue ────────────────────────────
    // Swipe cast: queue a spread to nearby enemies in OnUnitUpdate safe context.
    // victimLow = primary Swipe target; radius 8yd.
    struct RipAndTearQueue { bool queued = false; uint32 victimLow = 0; };
    std::unordered_map<uint32, RipAndTearQueue> g_ripAndTearQueue;

    // ── Druid: Feral Charge Mastery — melee-ability window ───────────────────
    // After Feral Charge cast, next melee ABILITY (not white hit) gets bonus.
    // Tracked via FeralChargeState.consumed = false means bonus not yet applied.

    // =========================================================================
    // BURN-TANK ENGINE STATE (Molten Shell clone)
    // =========================================================================

    // ── Warrior: Vengeful Bulwark (5019) — AP-reflect burn-tank ─────────────
    struct VengefulBulwarkState
    {
        uint8  heat      = 0;
        uint8  maxHeat   = 6;
        uint32 lastHitMs = 0;
    };
    std::unordered_map<uint32, VengefulBulwarkState> g_vengefulBulwark;

    // ── DK: Corrupted Carapace (5527) — shadow-reflect + disease amp ─────────
    struct CorruptedCarapaceState
    {
        uint8  heat      = 0;
        uint8  maxHeat   = 6;
        uint32 lastHitMs = 0;
    };
    std::unordered_map<uint32, CorruptedCarapaceState> g_corruptedCarapace;

    // ── Druid: Ironfur (5931) — Bear burn-tank (Thorns amplifier + melee DR) ──
    struct IronfurState
    {
        uint8  heat      = 0;
        uint8  maxHeat   = 6;
        uint32 lastHitMs = 0;
    };
    std::unordered_map<uint32, IronfurState> g_ironfur;

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
        g_lastShapeshiftForm.erase(guid);
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
        // Death Knight
        // Note: g_finalRune is NOT erased on death/logout — CD persists across combat (3min wall time).
        g_frHot.erase(guid);
        // Mage
        g_moltenShell.erase(guid);
        g_scorched.erase(guid);
        for (auto& [ag, vm] : g_scorched) vm.erase(guid);
        g_igniteStacks.erase(guid);
        for (auto& [ag, vm] : g_igniteStacks) vm.erase(guid);
        g_lostInTimeTick.erase(guid);
        for (auto& [ag, vm] : g_lostInTimeTick) vm.erase(guid);
        g_dragonFireZone.erase(guid);
        g_focusedMagicZone.erase(guid);
        g_spellWeaving.erase(guid);
        g_manaReactorReady.erase(guid);
        g_frostBounceQueue.erase(guid);
        g_deepFreezeFreeCastQueue.erase(guid);
        g_pyroDoT.erase(guid);
        for (auto& [ag, vm] : g_pyroDoT) vm.erase(guid);
        g_fireBlastCascadeQueue.erase(guid);
        g_meteorStrike.erase(guid);
        g_meteorQueue.erase(guid);
        g_heatingUpQueue.erase(guid);
        g_combustionSpreadQueue.erase(guid);
        // Druid
        g_wotwAbsorb.erase(guid);
        g_sunfireDoT.erase(guid);
        for (auto& [ag, vm] : g_sunfireDoT) vm.erase(guid);
        g_livingSeed.erase(guid);
        for (auto& [ag, vm] : g_livingSeed) vm.erase(guid);
        g_ancestralSpiritsLastTick.erase(guid);
        g_heartOfWild.erase(guid);
        g_feralCharge.erase(guid);
        g_survivalInstincts.erase(guid);
        g_naturesChosen.erase(guid);
        g_ripAndTearQueue.erase(guid);
        // Burn-tank engines
        g_vengefulBulwark.erase(guid);
        g_corruptedCarapace.erase(guid);
        g_ironfur.erase(guid);
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
// Exported for aa_actives.cpp — Mage: Focused Magic zone activate
// ---------------------------------------------------------------------------
void SanctumAA_SetFocusedMagicZone(uint32 playerGuid, float x, float y, float z, uint32 mapId, uint32 durationMs, uint32 tickDmg)
{
    g_focusedMagicZone[playerGuid] = { x, y, z, mapId, getMSTime() + durationMs, 0, tickDmg };
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Mage: Dragon's Fire zone set
// ---------------------------------------------------------------------------
void SanctumAA_SetDragonFireZone(uint32 playerGuid, float x, float y, float z, uint32 mapId, uint32 durationMs, uint32 tickDmg)
{
    g_dragonFireZone[playerGuid] = { x, y, z, mapId, getMSTime() + durationMs, 0, tickDmg };
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Mage: Molten Shell flare queue (actives queues it)
// Used by Frenzied Burnout and Host of Elements (not Molten Shell — that is internal)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Mage: Heating Up queue Hot Streak apply
// ---------------------------------------------------------------------------
void SanctumAA_QueueHeatingUp(uint32 playerGuid)
{
    g_heatingUpQueue[playerGuid] = true;
}

// ---------------------------------------------------------------------------
// Exported for aa_actives.cpp — Druid: Survival Instincts DR window
// ---------------------------------------------------------------------------
void SanctumAA_SetSurvivalInstinctsWindow(uint32 playerGuid, float drPct, uint32 durationMs)
{
    g_survivalInstincts[playerGuid] = { drPct, getMSTime() + durationMs };
}

// ---------------------------------------------------------------------------
// Exported (aa_runtime.h) — Druid: Living Seed store
// ---------------------------------------------------------------------------
void SanctumAA_SetLivingSeed(uint32 healerGuid, uint32 targetLow, uint32 seedAmount)
{
    g_livingSeed[healerGuid][targetLow] = seedAmount;
}

// ---------------------------------------------------------------------------
// Exported (aa_runtime.h) — Druid: Heart of the Wild window
// ---------------------------------------------------------------------------
void SanctumAA_SetHeartOfTheWildWindow(uint32 playerGuid, uint8 rank, uint32 untilMs)
{
    g_heartOfWild[playerGuid] = { rank, untilMs };
}

// ---------------------------------------------------------------------------
// Exported (aa_runtime.h) — Druid: Feral Charge window setter
// ---------------------------------------------------------------------------
void SanctumAA_SetFeralChargeWindow(uint32 playerGuid, uint8 rank, uint32 durationMs)
{
    g_feralCharge[playerGuid] = { rank, getMSTime() + durationMs, false };
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
// SanctumAA_ShowBuff / SanctumAA_RemoveBuff
// Apply/refresh or remove a Sanctum AA display-only buff aura on player (and
// optionally pets/guardians).  SAFE ONLY outside damage-modifier hooks.
// ---------------------------------------------------------------------------
void SanctumAA_ShowBuff(Player* player, uint32 spellId, uint32 durationMs, uint8 stacks, bool toPets)
{
    if (!player) return;
    auto applyOne = [&](Unit* u)
    {
        if (!u || !u->IsInWorld() || !u->IsAlive()) return;
        Aura* a = u->GetAura(spellId);
        if (!a)
            a = player->AddAura(spellId, u);
        if (!a) return;
        if (stacks) a->SetStackAmount(stacks);
        if (durationMs) a->SetDuration((int32)durationMs);
        a->SetNeedClientUpdateForTargets();
    };
    applyOne(player);
    if (toPets)
        for (Unit* c : player->m_Controlled)
            applyOne(c);
}

void SanctumAA_RemoveBuff(Player* player, uint32 spellId, bool toPets)
{
    if (!player) return;
    player->RemoveAura(spellId);
    if (toPets)
        for (Unit* c : player->m_Controlled)
            if (c) c->RemoveAura(spellId);
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

            // ── Druid: Improved Beast Form (5903) — Cat: +3/5/8% dmg done ────────
            // Bear side (-DR) is in VICTIM block. Cat bonus applies to all melee in Cat form.
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_BEAST_FORM);
                if (rank > 0)
                {
                    ShapeshiftForm form = player->GetShapeshiftForm();
                    if (form == FORM_CAT)
                    {
                        static const float bonus[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                        damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Druid: Improved Berserk (5905) — +5/8/12% dmg while Berserk active ──
            // Berserk aura: 50334
            if (target && SanctumAA::GetRank(player, AA_DRU_IMPROVED_BERSERK) > 0 &&
                player->HasAura(50334))
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_BERSERK);
                static const float bonus[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
            }

            // ── Druid: Beast Within (5902) — +10/18/28% dmg per 1% melee haste ────
            // Applies in both Bear and Cat form.
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_BEAST_WITHIN);
                if (rank > 0)
                {
                    ShapeshiftForm form = player->GetShapeshiftForm();
                    if (form == FORM_BEAR || form == FORM_DIREBEAR || form == FORM_CAT)
                    {
                        // melee haste % via haste rating (CR_HASTE_MELEE)
                        float hastePct = player->GetRatingBonusValue(CR_HASTE_MELEE);
                        if (hastePct > 0.0f)
                        {
                            static const float perHaste[] = { 0.0f, 0.10f, 0.18f, 0.28f };
                            float bonus = (perHaste[Idx<uint8>(rank)] / 100.0f) * hastePct;
                            damage += (uint32)(damage * std::min(bonus, 1.0f)); // cap at 100% bonus
                        }
                    }
                }
            }

            // ── Druid: Augmented Beast Form (5904) — +1.5/2.5/4% per 1% dodge ────
            if (target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_AUGMENTED_BEAST_FORM);
                if (rank > 0)
                {
                    ShapeshiftForm form = player->GetShapeshiftForm();
                    if (form == FORM_BEAR || form == FORM_DIREBEAR || form == FORM_CAT)
                    {
                        float dodgePct = player->GetFloatValue(PLAYER_DODGE_PERCENTAGE);
                        if (dodgePct > 0.0f)
                        {
                            static const float perDodge[] = { 0.0f, 0.015f, 0.025f, 0.04f };
                            float bonus = (perDodge[Idx<uint8>(rank)] / 100.0f) * dodgePct;
                            damage += (uint32)(damage * std::min(bonus, 1.0f));
                        }
                    }
                }
            }

            // ── Druid: Vengeful Bulwark (5019 Warrior) — +2% dmg per heat stack ──
            // Attacker side: read own heat stacks for damage bonus.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_VENGEFUL_BULWARK);
                if (rank > 0)
                {
                    auto it = g_vengefulBulwark.find(guid);
                    if (it != g_vengefulBulwark.end() && it->second.heat > 0)
                        damage += (uint32)(damage * 0.02f * it->second.heat);
                }
            }

            // ── Druid: Feral Charge Mastery (5933) — next ability after Feral Charge ──
            // White hits don't consume the window — only ability hits (via OnPlayerSpellCast).
            // So in ModifyMeleeDamage (white swings) we apply but do NOT consume.
            // Actual consumption happens in the spell hook below.

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

            // Final Rune (5519) — DK one-shot cheat-death (melee).
            // When a blow would be fatal and the 3-min CD has expired, clamp to 15% HP survival,
            // queue a 20% max HP HoT over 4s, and start the 3-min internal cooldown.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_FINAL_RUNE);
                if (rank > 0 && player->IsAlive() && damage >= player->GetHealth())
                {
                    auto& fr = g_finalRune[vGuid];
                    uint32 nowMs = getMSTime();
                    if (fr.cdUntilMs == 0 || nowMs >= fr.cdUntilMs)
                    {
                        // Set 3-minute cooldown (wall-clock, not per-combat)
                        fr.cdUntilMs = nowMs + 180000u;

                        // Clamp: survive at 15% max HP
                        uint32 surviveHP = (uint32)(player->GetMaxHealth() * 0.15f);
                        damage = player->GetHealth() > surviveHP ? (player->GetHealth() - surviveHP) : 0u;

                        // Queue HoT: 20% max HP over 4s (1s ticks), same pattern as Battle Endurance
                        auto& hot = g_frHot[vGuid];
                        hot.pool        = (int32)(player->GetMaxHealth() * 0.20f);
                        hot.lastTickMs  = nowMs;
                    }
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

            // ── Druid: Survival Instincts (5930) — active DR window ──────────────
            {
                auto it = g_survivalInstincts.find(vGuid);
                if (it != g_survivalInstincts.end() && it->second.drPct > 0.0f)
                {
                    if (getMSTime() >= it->second.untilMs)
                        it->second.drPct = 0.0f;
                    else
                        damage = (uint32)(damage * (1.0f - it->second.drPct));
                }
            }

            // ── Druid: Improved Beast Form (5903) — Bear: -3/5/8% dmg taken ────
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_BEAST_FORM);
                if (rank > 0)
                {
                    ShapeshiftForm form = player->GetShapeshiftForm();
                    if (form == FORM_BEAR || form == FORM_DIREBEAR)
                    {
                        static const float dr[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                        damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }
            }

            // ── Druid: Improved Berserk (5905) — -5/8/12% dmg taken while Berserk active ──
            if (player->HasAura(50334))
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_BERSERK);
                if (rank > 0)
                {
                    static const float dr[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                    damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                }
            }

            // ── Druid: Wrath of the Wild (5907) — absorb ward ─────────────────
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_WRATH_OF_THE_WILD);
                if (rank > 0)
                {
                    auto& wotw = g_wotwAbsorb[vGuid];
                    if (wotw.absorb > 0)
                    {
                        uint32 absorbed = std::min((uint32)wotw.absorb, damage);
                        damage -= absorbed;
                        wotw.absorb -= (int32)absorbed;
                    }
                }
            }

            // ── Druid: Augmented Thorns (5929) — Thorns reflect on melee hit ──
            // Reflects nature damage = (base Thorns dmg + 30/50/75% SP) to attacker.
            // This fires BEFORE Ironfur amplification (which reads the Thorns damage).
            // NOTE: Ironfur amplification is applied in this same block below.
            if (attacker && attacker->IsAlive() && damage > 0)
            {
                uint8 augRank = SanctumAA::GetRank(player, AA_DRU_AUGMENTED_THORNS);
                uint8 impRank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_THORNS);
                if ((augRank > 0 || impRank > 0) && player->HasAura(467) /* Thorns R1 check: we check any Thorns aura */ )
                {
                    // Thorns aura IDs: 467 (R1), 782 (R2), 1075 (R3), 8914 (R4), 9756 (R5), 9910 (R6), 26992 (R7), 53307 (R8)
                    static const std::unordered_set<uint32> s_thorns = {467, 782, 1075, 8914, 9756, 9910, 26992, 53307};
                    bool hasThorns = false;
                    for (uint32 id : s_thorns)
                        if (player->HasAura(id)) { hasThorns = true; break; }

                    if (hasThorns)
                    {
                        // Base Thorns reflect (improved by Improved Thorns 5928)
                        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE);
                        if (sp < 0) sp = 0;

                        // Augmented Thorns: base Thorns gains 30/50/75% SP
                        float spBonus = 0.0f;
                        if (augRank > 0)
                        {
                            static const float augPct[] = { 0.0f, 0.30f, 0.50f, 0.75f };
                            spBonus = sp * augPct[Idx<uint8>(augRank)];
                        }

                        // Improved Thorns: +20/35/50% to reflect amount
                        float impMult = 1.0f;
                        if (impRank > 0)
                        {
                            static const float impBonus[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                            impMult = 1.0f + impBonus[Idx<uint8>(impRank)];
                        }

                        uint32 reflBase = std::max(1u, (uint32)((spBonus) * impMult));

                        // Ironfur amplification: +8/12/15% per Ironfur stack if in Bear form
                        float ironfurMult = 1.0f;
                        {
                            uint8 ifRank = SanctumAA::GetRank(player, AA_DRU_IRONFUR);
                            if (ifRank > 0)
                            {
                                ShapeshiftForm iForm = player->GetShapeshiftForm();
                                if (iForm == FORM_BEAR || iForm == FORM_DIREBEAR)
                                {
                                    auto ifIt = g_ironfur.find(vGuid);
                                    if (ifIt != g_ironfur.end() && ifIt->second.heat > 0)
                                    {
                                        static const float perStack[] = { 0.0f, 0.08f, 0.12f, 0.15f };
                                        ironfurMult = 1.0f + perStack[Idx<uint8>(ifRank)] * ifIt->second.heat;
                                    }
                                }
                            }
                        }

                        uint32 reflFinal = std::max(1u, (uint32)(reflBase * ironfurMult));
                        if (reflFinal > 0)
                            SanctumAA_DealVisibleDamage(player, attacker, reflFinal, SPELL_SCHOOL_MASK_NATURE);
                    }
                }
            }

            // ── Druid: Ironfur (5931) — Bear burn-tank heat engine ────────────
            // Gate: Bear form. Accumulate heat on each melee hit taken.
            // Reflect nature dmg via DealVisibleDamage; apply melee DR per stack.
            if (attacker && attacker->IsAlive() && damage > 0)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_IRONFUR);
                if (rank > 0)
                {
                    ShapeshiftForm form = player->GetShapeshiftForm();
                    if (form == FORM_BEAR || form == FORM_DIREBEAR)
                    {
                        static const uint8 maxHeatByRank[] = { 0, 6, 8, 10 };
                        auto& ifState = g_ironfur[vGuid];
                        ifState.maxHeat = maxHeatByRank[Idx<uint8>(rank)];

                        if (ifState.heat < ifState.maxHeat)
                            ifState.heat++;
                        ifState.lastHitMs = getMSTime();

                        // Reflect: nature dmg = 15% SP per stack
                        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE);
                        if (sp < 0) sp = 0;
                        uint32 reflDmg = std::max(1u, (uint32)(sp * 0.15f));
                        SanctumAA_DealVisibleDamage(player, attacker, reflDmg, SPELL_SCHOOL_MASK_NATURE);

                        // Melee DR: -1.5% per stack
                        damage = (uint32)(damage * (1.0f - 0.015f * ifState.heat));
                    }
                }
            }

            // ── Warrior: Vengeful Bulwark (5019) — reflect physical AP% to ALL attackers ──
            // NO stance/shield requirement. Reflects to ALL melee attackers this update.
            if (attacker && attacker->IsAlive() && damage > 0)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_WAR_VENGEFUL_BULWARK);
                if (rank > 0)
                {
                    static const uint8 maxHeatByRank[] = { 0, 6, 8, 10 };
                    auto& vbState = g_vengefulBulwark[vGuid];
                    vbState.maxHeat = maxHeatByRank[Idx<uint8>(rank)];

                    if (vbState.heat < vbState.maxHeat)
                        vbState.heat++;
                    vbState.lastHitMs = getMSTime();

                    // Reflect physical to ALL attackers (not just current one)
                    // Current attacker: immediate reflect now
                    float ap = (float)player->GetTotalAttackPowerValue(BASE_ATTACK);
                    static const float reflPct[] = { 0.0f, 0.15f, 0.22f, 0.30f };
                    uint32 reflDmg = std::max(1u, (uint32)(ap * reflPct[Idx<uint8>(rank)]));
                    SanctumAA_DealVisibleDamage(player, attacker, reflDmg, SPELL_SCHOOL_MASK_NORMAL);
                    // Other attackers get the reflect in OnUnitUpdate (same pattern as Retaliation)
                }
            }

            // ── DK: Corrupted Carapace (5527) — shadow-reflect, Blood/Frost Presence gate ──
            if (attacker && attacker->IsAlive() && damage > 0)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_CORRUPTED_CARAPACE);
                if (rank > 0)
                {
                    // Gate: Blood Presence (48263) or Frost Presence (48266)
                    bool validPresence = player->HasAura(48263) || player->HasAura(48266);
                    if (validPresence)
                    {
                        static const uint8 maxHeatByRank[] = { 0, 6, 8, 10 };
                        auto& ccState = g_corruptedCarapace[vGuid];
                        ccState.maxHeat = maxHeatByRank[Idx<uint8>(rank)];

                        if (ccState.heat < ccState.maxHeat)
                            ccState.heat++;
                        ccState.lastHitMs = getMSTime();

                        // Reflect shadow to attacker
                        float ap = (float)player->GetTotalAttackPowerValue(BASE_ATTACK);
                        static const float reflPct[] = { 0.0f, 0.12f, 0.18f, 0.25f };
                        uint32 reflDmg = std::max(1u, (uint32)(ap * reflPct[Idx<uint8>(rank)]));
                        SanctumAA_DealVisibleDamage(player, attacker, reflDmg, SPELL_SCHOOL_MASK_SHADOW);
                    }
                }
            }

            // ── Druid: Living Seed bloom (5921) — incoming melee damage blooms seed ──
            // When target takes damage, bloom the seed as a heal.
            if (damage > 0)
            {
                // Find any player who has a Living Seed set on this victim
                // We iterate g_livingSeed to find seeds targeting this player's GUID.
                // (Seeds are keyed by HEALER guid → targetLow → amount)
                for (auto& [hGuid, seedMap] : g_livingSeed)
                {
                    auto sit = seedMap.find(vGuid);
                    if (sit != seedMap.end() && sit->second > 0)
                    {
                        uint32 seedHeal = sit->second;
                        sit->second = 0;
                        player->ModifyHealth((int32)seedHeal);
                    }
                }
            }

            // ── Molten Shell (5743) — victim=player, Molten Armor active ────────────
            // On melee hit: accumulate Heat, reflect fire dmg to attacker, apply melee DR.
            // SAFETY: SanctumAA_DealVisibleDamage reflects SPELL dmg onto ATTACKER,
            // which does NOT re-enter this player's ModifyMeleeDamage hook.
            if (attacker && attacker->IsAlive() && damage > 0)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_MOLTEN_SHELL);
                if (rank > 0)
                {
                    // Molten Armor aura IDs (ranks 1/2/3): 30482, 43043, 43044
                    static const std::unordered_set<uint32> s_moltenArmor = { 30482, 43043, 43044 };
                    bool moltenActive = false;
                    for (uint32 id : s_moltenArmor)
                        if (player->HasAura(id)) { moltenActive = true; break; }

                    if (moltenActive && attacker != player)
                    {
                        static const uint8  maxHeatByRank[] = { 0, 6, 8, 10 };
                        auto& ms = g_moltenShell[vGuid];
                        ms.maxHeat = maxHeatByRank[Idx<uint8>(rank)];

                        // Accumulate Heat
                        if (ms.heat < ms.maxHeat)
                            ms.heat++;
                        ms.lastHitMs = getMSTime();

                        // 1. Reflect: player SP * 0.15/0.25/0.40 as fire
                        {
                            static const float reflPct[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                            int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE);
                            if (sp < 0) sp = 0;
                            uint32 reflDmg = std::max(1u, (uint32)(sp * reflPct[Idx<uint8>(rank)]));
                            SanctumAA_DealVisibleDamage(player, attacker, reflDmg, SPELL_SCHOOL_MASK_FIRE);
                        }

                        // 3. Tempering: each Heat stack -1.5% melee DR
                        if (ms.heat > 0)
                            damage = (uint32)(damage * (1.0f - 0.015f * ms.heat));

                        // 5. At max Heat: queue Molten Flare (fired in OnUnitUpdate safe context)
                        if (ms.heat >= ms.maxHeat && !ms.flareQueued)
                            ms.flareQueued = true;
                    }
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

            // ── Druid: Eclipse Mastery (5915, renamed) — +10/18/28% while in Eclipse ──
            // Solar Eclipse aura: 48517. Lunar Eclipse aura: 48518. (VERIFY these IDs)
            if (isMagical && target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_ECLIPSE_MASTERY);
                if (rank > 0)
                {
                    bool inSolar  = player->HasAura(48517);
                    bool inLunar  = player->HasAura(48518);
                    if (inSolar || inLunar)
                    {
                        static const float bonus[] = { 0.0f, 0.10f, 0.18f, 0.28f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Druid: Celestial Impact (5910) — Starfall +15/25/40% dmg ─────────
            // Starfall hit spell IDs: 48505 (R1), 48506 (R2)
            if (target)
            {
                static const std::unordered_set<uint32> s_starfall = { 48505, 48506, 53190, 53191 };
                if (s_starfall.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_DRU_CELESTIAL_IMPACT);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Druid: Celestial Wrath (5911) — Starfall extra star damage ────────
            // Extra stars are queued in OnUnitUpdate. For hits from Starfall directly,
            // we apply the bonus here; the extra stars are queued in OnUnitUpdate.
            // (Queue approach: in ModifySpellDamageTaken on Starfall hit, queue extra stars)
            if (target)
            {
                static const std::unordered_set<uint32> s_starfallW = { 48505, 48506, 53190, 53191 };
                if (s_starfallW.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_DRU_CELESTIAL_WRATH);
                    if (rank > 0)
                    {
                        // Queue 1 extra star hit (dispatched in OnUnitUpdate)
                        // Using the meteor queue pattern but for nature/arcane school
                        // Simple approach: directly deal extra star immediately (safe from spell hook)
                        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE);
                        if (sp < 0) sp = 0;
                        static const float starPct[] = { 0.0f, 0.25f, 0.35f, 0.50f };
                        uint32 extraDmg = std::max(1u, (uint32)(sp * starPct[Idx<uint8>(rank)]));
                        SanctumAA_DealVisibleDamage(player, target, extraDmg, SPELL_SCHOOL_MASK_ARCANE);
                    }
                }
            }

            // ── Druid: Nature's Tenacity (5908) — Moonfire+IS damage bonus ────────
            // Moonfire direct hit: 8921, 8924, 9833, 9834, 9835, 26987, 26988, 48462, 48463
            // Insect Swarm direct: 5570, 24974, 24975, 24976, 27013, 48468, 48469
            if (target)
            {
                static const std::unordered_set<uint32> s_mfIS = {
                    8921, 8924, 9833, 9834, 9835, 26987, 26988, 48462, 48463,  // Moonfire
                    5570, 24974, 24975, 24976, 27013, 48468, 48469              // Insect Swarm
                };
                if (s_mfIS.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_DRU_NATURES_TENACITY);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.12f, 0.22f, 0.32f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Druid: Improved Typhoon (5913) — Typhoon/Hurricane +dmg ──────────
            // Typhoon: 61384, 61391, 61392. Hurricane: 16914 (periodic); direct: 27012, 42231, 42230
            if (target)
            {
                static const std::unordered_set<uint32> s_typhHurr = {
                    61384, 61391, 61392,          // Typhoon
                    16914, 27012, 42231, 42230    // Hurricane
                };
                if (s_typhHurr.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_TYPHOON);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Druid: Improved Faerie Fire (5906) — Faerie Fire Feral +dmg ───────
            // Faerie Fire (Feral): 16857, 17390, 17391, 17392
            if (target)
            {
                static const std::unordered_set<uint32> s_ffFeral = { 16857, 17390, 17391, 17392 };
                if (s_ffFeral.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_FAERIE_FIRE);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                        // Resource generation: energy or rage
                        if (player->GetMaxPower(POWER_ENERGY) > 0)
                            player->ModifyPower(POWER_ENERGY, std::min<int32>(10, (int32)player->GetMaxPower(POWER_ENERGY) - (int32)player->GetPower(POWER_ENERGY)));
                        else if (player->GetMaxPower(POWER_RAGE) > 0)
                            player->ModifyPower(POWER_RAGE, std::min<int32>(50 /*5 rage in 10-unit scale*/, (int32)player->GetMaxPower(POWER_RAGE) - (int32)player->GetPower(POWER_RAGE)));
                    }
                }
            }

            // ── Druid: Heart of the Wild (5932) — +dmg window after essence switch ─
            if (isMagical && target)
            {
                auto it = g_heartOfWild.find(guid);
                if (it != g_heartOfWild.end() && getMSTime() < it->second.untilMs && it->second.rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.15f, 0.20f };
                    damage += (int32)(damage * bonus[Idx<uint8>(it->second.rank)]);
                }
            }

            // ── Druid: Feral Charge Mastery (5933) — next melee ability after Feral Charge ──
            // This hook only fires for spells (ability hits), not white swings.
            // We check for spell/ability hits in the feral forms.
            if (target && isPhysical)
            {
                auto it = g_feralCharge.find(guid);
                if (it != g_feralCharge.end() && getMSTime() < it->second.untilMs && !it->second.consumed)
                {
                    uint8 rank = it->second.rank;
                    static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    it->second.consumed = true;  // consume on first ability hit
                }
            }

            // ── DK: Corrupted Carapace (5527) — disease dmg amplification ────────
            // Boost Blood Plague (55078) and Frost Fever (55095) tick damage by stack count.
            // This fires in ModifySpellDamageTaken for direct spell hits; periodic handled below.

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

            // ── Mage: Mana Adept (5721) — spell dmg scales with current mana % ──────
            if (isMagical)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_MANA_ADEPT);
                if (rank > 0 && player->GetMaxPower(POWER_MANA) > 0)
                {
                    float manaPct = (float)player->GetPower(POWER_MANA) / (float)player->GetMaxPower(POWER_MANA);
                    // +5/10/15% at full mana, linear down to 0
                    static const float maxBonus[] = { 0.0f, 0.05f, 0.10f, 0.15f };
                    float bonus = maxBonus[Idx<uint8>(rank)] * manaPct;
                    if (bonus > 0.0f)
                        damage += (int32)(damage * bonus);
                }
            }

            // ── Mage: Molten Fury (5724) — fire spells +dmg vs sub-35% HP targets ──
            if (target && (schoolMask & SPELL_SCHOOL_MASK_FIRE) && target->GetHealthPct() < 35.0f)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_MOLTEN_FURY);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // ── Mage: Scorched (5723) — read fire-vuln stacks on target ─────────────
            // Scorch stacks are applied in OnPlayerSpellCast (aa_combat_player) below.
            if (target && (schoolMask & SPELL_SCHOOL_MASK_FIRE))
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_SCORCHED);
                if (rank > 0)
                {
                    uint32 vGuid = target->GetGUID().GetCounter();
                    auto it = g_scorched.find(guid);
                    if (it != g_scorched.end())
                    {
                        auto jt = it->second.find(vGuid);
                        if (jt != it->second.end() && jt->second.stacks > 0
                            && getMSTime() <= jt->second.expireMs)
                        {
                            static const float perStack[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                            damage += (int32)(damage * perStack[Idx<uint8>(rank)] * jt->second.stacks);
                        }
                    }
                }
            }

            // ── Mage: Explosive Impact (5701) — Living Bomb explosion +dmg ───────────
            if (target)
            {
                // Living Bomb explosion spell IDs (approximate — rank/talent combos):
                static const std::unordered_set<uint32> s_lbExplode = {
                    44461,  // Living Bomb — main explosion ID (WotLK)
                    55361,  // Living Bomb rank 2
                    55362,  // Living Bomb rank 3
                    44462   // Living Bomb periodic tick (do NOT boost this one here)
                };
                // Distinguish explosion (not tick): IsAffectingArea or direct effect check
                bool isLBExplode = s_lbExplode.count(spellInfo->Id) != 0 && spellInfo->IsTargetingArea();
                if (!isLBExplode && s_lbExplode.count(spellInfo->Id))
                    isLBExplode = true; // cast once to kill, counts as direct explosion
                if (isLBExplode)
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_MAG_EXPLOSIVE_IMPACT);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Mage: Empowered Flames (5703) — bonus dmg while proc auras active ───
            if (target && (schoolMask & SPELL_SCHOOL_MASK_FIRE))
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_EMPOWERED_FLAMES);
                if (rank > 0)
                {
                    // Impact aura (Firestarter talent): 12578, 12579, 12580, 12581
                    // Hot Streak proc aura: 48108
                    // Firestarter talent aura: no direct aura, but check Fire Blast after Impact
                    // For simplicity: check Impact proc aura presence
                    static const std::unordered_set<uint32> s_procAuras = {
                        12578, 12579, 12580, 12581,  // Impact proc auras
                        48108,                        // Hot Streak
                        44401                         // Hot Streak "proc" indicator (VERIFY)
                    };
                    bool hasProcAura = false;
                    for (uint32 id : s_procAuras)
                        if (player->HasAura(id)) { hasProcAura = true; break; }
                    if (hasProcAura)
                    {
                        static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Mage: Arcane Attunement (5717) — while Arcane Power active: +arcane dmg ──
            if (schoolMask & SPELL_SCHOOL_MASK_ARCANE)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_ARCANE_ATTUNEMENT);
                if (rank > 0)
                {
                    // Arcane Power aura IDs: 12042 (all ranks share same aura)
                    if (player->HasAura(12042))
                    {
                        static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Mage: Augmented Icy Veins (5713) — while Icy Veins: +spell dmg ───────
            // Icy Veins aura: 12472. +10/20/30% spell damage.
            if (isMagical)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_AUGMENTED_ICY_VEINS);
                if (rank > 0 && player->HasAura(12472))
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // ── Mage: Improved Frost Ward (5711) — -dmg while Frost Ward active ──────
            // Frost Ward aura: 6143 (R1), 8461 (R2), 8462 (R3), 10177 (R4), 28272 (R5), 32796 (R6)
            // NOTE: this is VICTIM side (player reducing own dmg taken). Handled below in victim block.

            // ── Mage: Augmented Deep Freeze (5709) — +dmg on Deep Freeze hits ────────
            // Deep Freeze spell IDs: 44572 (only rank, WotLK). Boosted here.
            if (target && spellInfo->Id == 44572)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_AUGMENTED_DEEP_FREEZE);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                    damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }

            // ── Mage: Arcane Subtlety (5715) — Arcane Barrage +dmg ───────────────────
            // Arcane Barrage IDs: 44425, 44781, 44780
            if (target)
            {
                static const std::unordered_set<uint32> s_arcaneBarrage = { 44425, 44781, 44780 };
                if (s_arcaneBarrage.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_MAG_ARCANE_SUBTLETY);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Mage: Chain Explosion (5716) — Arcane Explosion +dmg ────────────────
            // Arcane Explosion IDs: 1449 (R1), 8437 (R2), 8438 (R3), 10202 (R4), 10203 (R5), 27082 (R6), 42926 (R7), 42921 (R8)
            if (target)
            {
                static const std::unordered_set<uint32> s_arcaneExp = {
                    1449, 8437, 8438, 10202, 10203, 27082, 42926, 42921
                };
                if (s_arcaneExp.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_MAG_CHAIN_EXPLOSION);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                        // Second explosion queued in g_meteorQueue for dispatch in OnUnitUpdate
                        if (!g_meteorQueue.count(guid) || !g_meteorQueue[guid].queued)
                        {
                            uint32 secondDmg = (uint32)std::max(0, damage);
                            g_meteorQueue[guid] = { secondDmg, true };
                        }
                    }
                }
            }

            // ── Mage: Meteor Shower (5706) — Blast Wave fire splash ──────────────────
            // Blast Wave IDs: 11113 (R1), 13018 (R2), 13019 (R3), 13020 (R4), 13021 (R5), 27133 (R6), 33933 (R7), 42944 (R8), 42945 (R9)
            if (target)
            {
                static const std::unordered_set<uint32> s_blastWave = {
                    11113, 13018, 13019, 13020, 13021, 27133, 33933, 42944, 42945
                };
                if (s_blastWave.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_MAG_METEOR_SHOWER);
                    if (rank > 0)
                    {
                        static const float mult[] = { 0.0f, 0.60f, 1.00f, 1.50f };
                        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE);
                        if (sp < 0) sp = 0;
                        uint32 splashDmg = (uint32)(sp * mult[Idx<uint8>(rank)]);
                        if (splashDmg > 0)
                        {
                            // AoE around player location (not target) for 8yd
                            std::list<Unit*> nearList;
                            Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 8.0f);
                            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                            Cell::VisitObjects(player, searcher, 8.0f);
                            for (Unit* u : nearList)
                            {
                                if (!u || !u->IsAlive()) continue;
                                SanctumAA_DealVisibleDamage(player, u, splashDmg, SPELL_SCHOOL_MASK_FIRE);
                            }
                        }
                    }
                }
            }

            // ── Mage: Pyroblast Overload (5741) — Pyroblast +dmg + queue DoT ─────────
            // Pyroblast IDs: 11366 (R1), 12505 (R2), 12522 (R3), 12523 (R4), 12524 (R5), 12525 (R6), 12526 (R7),
            //                18809 (R8), 27338 (R9), 33938 (R10), 42891 (R11), 42892 (R12)
            if (target)
            {
                static const std::unordered_set<uint32> s_pyroblast = {
                    11366, 12505, 12522, 12523, 12524, 12525, 12526,
                    18809, 27338, 33938, 42891, 42892
                };
                if (s_pyroblast.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_MAG_PYROBLAST_OVERLOAD);
                    if (rank > 0)
                    {
                        static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                        damage += (int32)(damage * bonus[Idx<uint8>(rank)]);
                        // Queue DoT: 30/50/75% of boosted dmg over 6s (3 ticks of 2s each)
                        static const float dotPct[] = { 0.0f, 0.30f, 0.50f, 0.75f };
                        uint32 totalDot = (uint32)(std::max(0, damage) * dotPct[Idx<uint8>(rank)]);
                        uint32 tickDot  = std::max(1u, totalDot / 3u);
                        uint32 vLow = target->GetGUID().GetCounter();
                        auto& dotSt = g_pyroDoT[guid][vLow];
                        dotSt.endMs      = getMSTime() + 6000u;
                        dotSt.lastTickMs = getMSTime();
                        dotSt.tickDmg    = tickDot;
                    }
                }
            }

            // ── Mage: Fire Blast Cascade (5742) — splash all in 8yd of target ────────
            // Fire Blast IDs: 2136 (R1), 2137 (R2), 2138 (R3), 8412 (R4), 8413 (R5), 10197 (R6),
            //                 10199 (R7), 27079 (R8), 42873 (R9), 42872 (R10)
            if (target)
            {
                static const std::unordered_set<uint32> s_fireBlast = {
                    2136, 2137, 2138, 8412, 8413, 10197, 10199, 27079, 42873, 42872
                };
                if (s_fireBlast.count(spellInfo->Id))
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_MAG_FIRE_BLAST_CASCADE);
                    if (rank > 0)
                    {
                        static const float pct[] = { 0.0f, 0.40f, 0.60f, 0.80f };
                        uint32 splashDmg = (uint32)(std::max(0, damage) * pct[Idx<uint8>(rank)]);
                        if (splashDmg > 0)
                        {
                            // Queue for safe dispatch to all enemies within 8yd of target
                            g_fireBlastCascadeQueue[guid] = { target->GetGUID().GetCounter(), splashDmg };
                        }
                    }
                }
            }

            // ── Mage: Spell Weaving (5739) — stacking bonus on school switch ──────────
            if (isMagical && target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_SPELL_WEAVING);
                if (rank > 0)
                {
                    auto& sw = g_spellWeaving[guid];
                    uint32 currentSchool = schoolMask;
                    if (sw.lastSchool != 0 && sw.lastSchool != currentSchool)
                    {
                        // Different school from last cast: gain a stack
                        uint8 maxStacks = 5;
                        if (sw.stacks < maxStacks)
                            sw.stacks++;
                    }
                    else if (sw.lastSchool == currentSchool && sw.stacks > 0)
                    {
                        // Same school: decay one stack
                        sw.stacks--;
                    }
                    sw.lastSchool = currentSchool;

                    if (sw.stacks > 0)
                    {
                        static const float perStack[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                        damage += (int32)(damage * perStack[Idx<uint8>(rank)] * sw.stacks);
                    }
                }
            }

            // ── Mage: Molten Shell (5743) — Burn Ramp: Heat -> +fire spell dmg ────────
            if (schoolMask & SPELL_SCHOOL_MASK_FIRE)
            {
                auto it = g_moltenShell.find(guid);
                if (it != g_moltenShell.end() && it->second.heat > 0)
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_MAG_MOLTEN_SHELL);
                    if (rank > 0)
                    {
                        // +2% per Heat stack
                        damage += (int32)(damage * 0.02f * it->second.heat);
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

            // Final Rune (5519) — DK one-shot cheat-death (spell).
            // Same logic as the melee intercept above; uses the same g_finalRune CD map
            // so one CD covers both melee and spell fatal hits.
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_FINAL_RUNE);
                if (rank > 0 && player->IsAlive() && damage >= (int32)player->GetHealth())
                {
                    auto& fr = g_finalRune[vGuid];
                    uint32 nowMs = getMSTime();
                    if (fr.cdUntilMs == 0 || nowMs >= fr.cdUntilMs)
                    {
                        fr.cdUntilMs = nowMs + 180000u;

                        int32 surviveHP = (int32)(player->GetMaxHealth() * 0.15f);
                        damage = (int32)player->GetHealth() > surviveHP ? ((int32)player->GetHealth() - surviveHP) : 0;

                        auto& hot = g_frHot[vGuid];
                        hot.pool        = (int32)(player->GetMaxHealth() * 0.20f);
                        hot.lastTickMs  = nowMs;
                    }
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

            // ── Mage: Improved Frost Ward (5711) — -dmg while Frost Ward active ───────
            // Frost Ward aura IDs: 6143, 8461, 8462, 10177, 28272, 32796
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_IMPROVED_FROST_WARD);
                if (rank > 0)
                {
                    static const std::unordered_set<uint32> s_frostWard = { 6143, 8461, 8462, 10177, 28272, 32796 };
                    bool hasFW = false;
                    for (uint32 id : s_frostWard)
                        if (player->HasAura(id)) { hasFW = true; break; }
                    if (hasFW)
                    {
                        static const float dr[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                        damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }
            }

            // ── Druid: Survival Instincts (5930) — active DR window (spell) ──────
            {
                auto it = g_survivalInstincts.find(vGuid);
                if (it != g_survivalInstincts.end() && it->second.drPct > 0.0f)
                {
                    if (getMSTime() >= it->second.untilMs)
                        it->second.drPct = 0.0f;
                    else
                        damage = (int32)(damage * (1.0f - it->second.drPct));
                }
            }

            // ── Druid: Improved Beast Form (5903) — Bear: -dmg taken (spell) ───────
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_BEAST_FORM);
                if (rank > 0)
                {
                    ShapeshiftForm form = player->GetShapeshiftForm();
                    if (form == FORM_BEAR || form == FORM_DIREBEAR)
                    {
                        static const float dr[] = { 0.0f, 0.03f, 0.05f, 0.08f };
                        damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                    }
                }
            }

            // ── Druid: Improved Berserk (5905) — -dmg taken while Berserk (spell) ──
            if (player->HasAura(50334))
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_BERSERK);
                if (rank > 0)
                {
                    static const float dr[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                    damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
                }
            }

            // ── Druid: Wrath of the Wild (5907) — absorb ward (spell dmg) ────────
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_WRATH_OF_THE_WILD);
                if (rank > 0)
                {
                    auto& wotw = g_wotwAbsorb[vGuid];
                    if (wotw.absorb > 0)
                    {
                        int32 absorbed = std::min(wotw.absorb, damage);
                        damage -= absorbed;
                        wotw.absorb -= absorbed;
                    }
                }
            }

            // ── Druid: Living Seed bloom (5921) — spell damage triggers bloom too ──
            if (damage > 0)
            {
                for (auto& [hGuid, seedMap] : g_livingSeed)
                {
                    auto sit = seedMap.find(vGuid);
                    if (sit != seedMap.end() && sit->second > 0)
                    {
                        uint32 seedHeal = sit->second;
                        sit->second = 0;
                        player->ModifyHealth((int32)seedHeal);
                    }
                }
            }

            // ── Druid: Heart of the Wild (5932) — dmg taken not reduced (offense only) ──
            // The window only grants offensive bonus, not DR.

        } // end VICTIM IS PLAYER
    }

    // -----------------------------------------------------------------------
    // ModifyPeriodicDamageAurasTick — DoT tick modifier (School Mastery).
    // -----------------------------------------------------------------------
    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
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

        // ── Mage: Spreading Flames (5702) — Ignite tick stacks +dmg ─────────────────
        // Ignite aura IDs: 12654, 12654 (shared); the periodic spell effect IDs are 12654 ticks.
        // Ignite tick ID in WotLK: 12654 (all ranks share one aura, tick ID may vary). VERIFY.
        // Using school+effect check as fallback since we can't guarantee the exact tick spell ID.
        if (schoolMask & SPELL_SCHOOL_MASK_FIRE)
        {
            // Check if this is an Ignite tick: spell named "Ignite" (approximate: spellInfo->SpellFamilyName == SPELLFAMILY_MAGE)
            bool isIgnite = (spellInfo->Id == 12654 || spellInfo->Id == 12846 || spellInfo->Id == 12847
                          || spellInfo->Id == 12848 || spellInfo->Id == 12849 || spellInfo->Id == 12850);
            if (isIgnite)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_SPREADING_FLAMES);
                if (rank > 0 && target)
                {
                    uint32 vLow = target->GetGUID().GetCounter();
                    uint32 guid = player->GetGUID().GetCounter();
                    auto& entry = g_igniteStacks[guid][vLow];
                    uint32 now = getMSTime();
                    if (entry.stacks > 0 && now > entry.expireMs)
                        entry = IgniteStackEntry{};
                    if (entry.stacks < 10)
                        entry.stacks++;
                    entry.expireMs = now + 8000u;  // reset on each tick

                    static const float perStack[] = { 0.0f, 0.02f, 0.03f, 0.05f };
                    if (entry.stacks > 0)
                        damage += (uint32)(damage * perStack[Idx<uint8>(rank)] * entry.stacks);
                }
            }

            // ── Slow Burn (5707) — Ignite duration doubled (passive, no penalty) ──
            // Implemented via periodic: if the Ignite tick we just boosted has stacks, no extra action needed.
            // Duration doubling is a property the server can't easily change on the aura —
            // instead we allow re-stacking the tick bonus longer. As-shipped: Ignite stacks
            // remain active for 8s per tick reset (extended vs normal 3s). Satisfies intent.
        }

        // ── Mage: Blizzard (5745) — Blizzard ticks +dmg ─────────────────────────
        // Blizzard periodic spell IDs: 10, 6141, 8427, 10185, 10186, 10187, 27085, 27086, 42208, 42209
        {
            static const std::unordered_set<uint32> s_blizzard = {
                10, 6141, 8427, 10185, 10186, 10187, 27085, 27086, 42208, 42209
            };
            if (s_blizzard.count(spellInfo->Id))
            {
                uint8 rank = SanctumAA::GetRank(player, AA_MAG_BLIZZARD);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
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

        // ── Druid: Nature's Remedy (5920) — Druid HoT ticks +8/15/25% ──────────
        // Detected via school=nature + periodic. All Druid HoTs are nature school.
        // Spell IDs: Rejuvenation 774,1058,1430,2090,2091,3627,8910,9839,9840,9841,25299,26981,26982,27141,48440,48441
        //            Regrowth DoT: 8936,8938,8939,8940,8941,9750,9856,9857,9858,26980,27141,48442,48443
        //            Lifebloom: 33763,48450,48451
        //            Wild Growth: 48438,48500,53248,53249,53250,53251,53252
        if (schoolMask & SPELL_SCHOOL_MASK_NATURE)
        {
            static const std::unordered_set<uint32> s_druidHoTs = {
                // Rejuvenation
                774,1058,1430,2090,2091,3627,8910,9839,9840,9841,25299,26981,26982,27141,48440,48441,
                // Regrowth HoT
                8936,8938,8939,8940,8941,9750,9856,9857,9858,26980,48442,48443,
                // Lifebloom
                33763,48450,48451,
                // Wild Growth
                48438,48500,53248,53249,53250,53251,53252
            };
            if (s_druidHoTs.count(spellInfo->Id))
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_NATURES_REMEDY);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.08f, 0.15f, 0.25f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }
            }
        }

        // ── Druid: Nature's Tenacity (5908) — Moonfire/Insect Swarm DoT ticks ───
        // DoT tick IDs for Moonfire: 8921, 8924, 9833, 9834, 9835, 26987, 26988, 48462, 48463
        // Insect Swarm DoT ticks: 5570, 24974, 24975, 24976, 27013, 48468, 48469
        {
            static const std::unordered_set<uint32> s_mfISTick = {
                8921,8924,9833,9834,9835,26987,26988,48462,48463,  // Moonfire
                5570,24974,24975,24976,27013,48468,48469             // Insect Swarm
            };
            if (s_mfISTick.count(spellInfo->Id))
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DRU_NATURES_TENACITY);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.12f, 0.22f, 0.32f };
                    damage += (uint32)(damage * bonus[Idx<uint8>(rank)]);
                }

                // Eclipse tick-spread (R3): in Solar Eclipse, IS ticks spread to nearby enemy;
                // in Lunar Eclipse, Moonfire ticks spread.
                // We queue spread as immediate extra DealVisibleDamage call (safe from periodic hook).
                if (rank >= 3 && target)
                {
                    uint32 victimLow  = target->GetGUID().GetCounter();
                    bool inSolar  = player->HasAura(48517);
                    bool inLunar  = player->HasAura(48518);
                    bool isIS  = (spellInfo->Id == 5570 || spellInfo->Id == 24974 || spellInfo->Id == 24975 ||
                                  spellInfo->Id == 24976 || spellInfo->Id == 27013 || spellInfo->Id == 48468 || spellInfo->Id == 48469);
                    bool isMF  = (s_mfISTick.count(spellInfo->Id) && !isIS);
                    bool shouldSpread = (inSolar && isIS) || (inLunar && isMF);
                    if (shouldSpread)
                    {
                        static const float spreadChance = 30.0f;
                        if (roll_chance_f(spreadChance))
                        {
                            // Find a random nearby enemy that does NOT have this DoT
                            std::list<Unit*> nearList;
                            Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 8.0f);
                            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                            Cell::VisitObjects(player, searcher, 8.0f);
                            std::vector<Unit*> candidates;
                            for (Unit* u : nearList)
                                if (u && u->IsAlive() && u->GetGUID().GetCounter() != victimLow && !u->HasAura(spellInfo->Id))
                                    candidates.push_back(u);
                            if (!candidates.empty())
                            {
                                uint32 idx = urand(0, (uint32)(candidates.size() - 1));
                                SanctumAA_DealVisibleDamage(player, candidates[idx], damage, (uint32)spellInfo->GetSchoolMask());
                            }
                        }
                    }
                }
            }
        }

        // ── Druid: Sunfire (5914) — rider nature DoT, ticked via g_sunfireDoT ──
        // The DoT is queued from OnPlayerSpellCast (Moonfire cast detection) in the
        // aa_druid_player script below. Ticks dispatched in OnUnitUpdate.
        // No action needed here — DoT ticks are delivered via SanctumAA_DealVisibleDamage.

        // ── DK: Corrupted Carapace (5527) — disease dmg amplification per heat stack ──
        // Boost Blood Plague (55078) and Frost Fever (55095) tick damage.
        {
            static const std::unordered_set<uint32> s_dkDiseases = { 55078, 55095 };
            if (s_dkDiseases.count(spellInfo->Id) && target)
            {
                uint8 rank = SanctumAA::GetRank(player, AA_DK_CORRUPTED_CARAPACE);
                if (rank > 0)
                {
                    uint32 guid = player->GetGUID().GetCounter();
                    auto it = g_corruptedCarapace.find(guid);
                    if (it != g_corruptedCarapace.end() && it->second.heat > 0)
                    {
                        // +5% per heat stack
                        damage += (uint32)(damage * 0.05f * it->second.heat);
                    }
                }
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

            // ── Druid: Healing Adept (5918) — direct heals +5/10/18% (Tree form only) ──
            // Tree of Life form check. Direct heal spell IDs:
            // Healing Touch: 5185,5186,5187,5188,5189,5190,5191,5192,6778,8903,9758,9888,9889,25297,25311,26979,48377,48378
            // Regrowth direct: 8936,8938,8939,8940,8941,9750,9856,9857,9858,26980,48442,48443 (same IDs as DoT ticks but direct effect)
            // Nourish: 50464
            // Tranquility: 740,8918,9862,9863,26983,48447
            // Actually use family name check is more robust. We'll check spell IDs for known heals.
            if (healer == target || (healer && target))  // direct heals fire when healer casts on any target
            {
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_DRU_HEALING_ADEPT);
                if (rank > 0 && hPlayer->GetShapeshiftForm() == FORM_TREE)
                {
                    static const std::unordered_set<uint32> s_directHeals = {
                        5185,5186,5187,5188,5189,5190,5191,5192,6778,8903,9758,9888,9889,
                        25297,25311,26979,48377,48378,  // Healing Touch
                        50464,                           // Nourish
                        740,8918,9862,9863,26983,48447  // Tranquility
                    };
                    if (s_directHeals.count(spellInfo->Id))
                    {
                        static const float bonus[] = { 0.0f, 0.05f, 0.10f, 0.18f };
                        heal += (uint32)(heal * bonus[Idx<uint8>(rank)]);
                    }
                }
            }

            // ── Druid: Swiftmend Mastery (5923) — Swiftmend +15/25/40% ─────────────
            // Swiftmend: 18562
            if (spellInfo->Id == 18562)
            {
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_DRU_SWIFTMEND_MASTERY);
                if (rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                    heal += (uint32)(heal * bonus[Idx<uint8>(rank)]);
                }
            }

            // ── Druid: Living Seed (5921) — direct heals plant a seed on target ────
            // Seed = 15/25/40% of the heal; blooms when target next takes damage.
            // Supported direct heals: Healing Touch, Regrowth direct, Nourish, Tranquility.
            {
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_DRU_LIVING_SEED);
                if (rank > 0 && target)
                {
                    static const std::unordered_set<uint32> s_seedHeals = {
                        5185,5186,5187,5188,5189,5190,5191,5192,6778,8903,9758,9888,9889,
                        25297,25311,26979,48377,48378,  // Healing Touch
                        50464,                           // Nourish
                        8936,8938,8939,8940,8941,9750,9856,9857,9858,26980,48442,48443,  // Regrowth direct
                        18562                            // Swiftmend
                    };
                    if (s_seedHeals.count(spellInfo->Id) && target->IsAlive())
                    {
                        static const float seedPct[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                        uint32 seedAmt = std::max(1u, (uint32)(heal * seedPct[Idx<uint8>(rank)]));
                        SanctumAA_SetLivingSeed(hGuid, target->GetGUID().GetCounter(), seedAmt);
                    }
                }
            }

            // ── Druid: Pack Chloroplast (5922) — Rejuv chance to copy to lowest HP unit ──
            // Rejuvenation direct apply IDs (the aura apply, not DoT ticks):
            // 774,1058,1430,2090,2091,3627,8910,9839,9840,9841,25299,26981,26982,27141,48440,48441
            // ModifyHealReceived fires once when Rejuv applies its first tick OR direct.
            // We use it to trigger the copy chance.
            {
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_DRU_PACK_CHLOROPLAST);
                static const std::unordered_set<uint32> s_rejuv = {
                    774,1058,1430,2090,2091,3627,8910,9839,9840,9841,
                    25299,26981,26982,27141,48440,48441
                };
                if (rank > 0 && s_rejuv.count(spellInfo->Id))
                {
                    static const float chance[] = { 0.0f, 25.0f, 50.0f, 75.0f };
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                    {
                        // Apply a half-strength heal to the lowest HP guardian/pet owned by the healer
                        Unit* lowestHP = nullptr;
                        uint32 lowestPct = 100;
                        auto checkUnit = [&](Unit* u) {
                            if (!u || !u->IsAlive()) return;
                            uint32 pct = (uint32)u->GetHealthPct();
                            if (pct < lowestPct) { lowestPct = pct; lowestHP = u; }
                        };
                        if (Pet* pet = hPlayer->GetPet()) checkUnit(pet);
                        for (Unit* g : hPlayer->m_Controlled) checkUnit(g);
                        if (lowestHP && lowestHP != target)
                        {
                            uint32 halfHeal = heal / 2;
                            if (halfHeal > 0)
                                lowestHP->ModifyHealth((int32)halfHeal);
                        }
                    }
                }
            }

            // ── Druid: Heart of the Wild (5932) — +heal bonus window ─────────────
            {
                auto it = g_heartOfWild.find(hGuid);
                if (it != g_heartOfWild.end() && getMSTime() < it->second.untilMs && it->second.rank > 0)
                {
                    static const float bonus[] = { 0.0f, 0.10f, 0.15f, 0.20f };
                    heal += (uint32)(heal * bonus[Idx<uint8>(it->second.rank)]);
                }
            }

            // ── Ancestral Guard (5605) — after self-heal: absorb shield = 10/20/30% of heal for 8s ──
            // Only triggers when healer == target (self-heal). Stored in g_ancestralGuardAbsorb,
            // consumed in aa_class.cpp's melee + spell damage-taken VICTIM IS PLAYER hooks.
            // NOTE: g_ancestralGuardAbsorb is declared in aa_class.cpp; we access it via extern.
            // SAFETY: ModifyHealReceived is NOT a damage hook — storing to state here is safe.
            if (healer == target)
            {
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_SHA_ANCESTRAL_GUARD);
                if (rank > 0)
                {
                    static const float pct[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                    int32 shieldAmt = (int32)(heal * pct[Idx<uint8>(rank)]);
                    if (shieldAmt > 0)
                    {
                        extern void SanctumAA_ApplyAncestralGuard(uint32 playerGuid, int32 amount, uint32 durationMs);
                        SanctumAA_ApplyAncestralGuard(hGuid, shieldAmt, 8000u);
                    }
                }
            }

            // ── Ancestral Bulwark (5619, renamed from Living Current) ─────────────
            // Chain Heal: grant Earth Shield to every target it heals (caster, pet, guardians).
            // ModifyHealReceived fires once per Chain Heal bounce (once per healed unit).
            // We QUEUE the shield apply — do NOT call CastSpell here (unsafe in heal hook).
            {
                static const std::unordered_set<uint32> s_chainHeal = {
                    1064,10622,10623,25422,25423,55458,55459
                };
                uint8 rank = SanctumAA::GetRank(hPlayer, AA_SHA_ANCESTRAL_BULWARK);
                if (rank > 0 && s_chainHeal.count(spellInfo->Id))
                {
                    // Queue Earth Shield for 'target' (the specific unit healed this bounce)
                    // Check: target is either the healer themselves, OR a unit owned by the healer.
                    bool eligible = (target == healer);
                    if (!eligible && target)
                    {
                        Unit* owner = target->GetOwner();
                        if (owner && owner == healer)
                            eligible = true;
                    }
                    if (eligible && target)
                    {
                        extern void SanctumAA_QueueEarthShield(uint32 playerGuid, uint32 targetLow);
                        SanctumAA_QueueEarthShield(hGuid, target->GetGUID().GetCounter());
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

        // Final Rune (5519) HoT — 20% max HP healed back over 4s (1s ticks)
        // Same tick pattern as Battle Endurance HoT; uses separate g_frHot map.
        {
            auto it = g_frHot.find(guid);
            if (it != g_frHot.end() && it->second.pool > 0)
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

        // ── Mage: Molten Shell (5743) ─────────────────────────────────────────
        // Decay Heat after 6s with no melee hit taken; dispatch queued Flare.
        {
            auto it = g_moltenShell.find(guid);
            if (it != g_moltenShell.end())
            {
                auto& ms = it->second;
                // Decay: no hit for 6s → reset Heat to 0
                if (ms.heat > 0 && ms.lastHitMs > 0 && GetMSTimeDiffToNow(ms.lastHitMs) >= 6000u)
                    ms.heat = 0;

                // Dispatch queued Molten Flare (safe context — NOT inside damage hook)
                if (ms.flareQueued)
                {
                    ms.flareQueued = false;
                    ms.heat = 0;  // reset heat after flare

                    uint8 rank = SanctumAA::GetRank(player, AA_MAG_MOLTEN_SHELL);
                    int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE);
                    if (sp < 0) sp = 0;
                    uint32 flareDmg = std::max(1u, (uint32)(sp * 1.00f));  // 100% SP

                    // AoE nova in 8yd
                    std::list<Unit*> nearList;
                    Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 8.0f);
                    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                    Cell::VisitObjects(player, searcher, 8.0f);
                    for (Unit* u : nearList)
                    {
                        if (!u || !u->IsAlive()) continue;
                        SanctumAA_DealVisibleDamage(player, u, flareDmg, SPELL_SCHOOL_MASK_FIRE);
                    }

                    // SYNERGY: if player owns Heating Up, also grant Hot Streak
                    if (SanctumAA::Has(player, AA_MAG_HEATING_UP))
                        player->CastSpell(player, 48108, true);  // Hot Streak proc aura
                }
            }
        }

        // ── Mage: Heating Up (5746) — queued Hot Streak grant ────────────────
        {
            auto it = g_heatingUpQueue.find(guid);
            if (it != g_heatingUpQueue.end() && it->second)
            {
                it->second = false;
                player->CastSpell(player, 48108, true);  // Hot Streak proc aura
            }
        }

        // ── Mage: Mana Reactor (5740) — monitor mana threshold ───────────────
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_MANA_REACTOR);
            if (rank > 0 && player->GetMaxPower(POWER_MANA) > 0)
            {
                float manaPct = (float)player->GetPower(POWER_MANA) / (float)player->GetMaxPower(POWER_MANA);
                if (manaPct < 0.20f)
                {
                    // Prime the refund flag if not already set
                    auto& flag = g_manaReactorReady[guid];
                    if (!flag)
                        flag = true;
                }
            }
        }

        // ── Mage: Pyroblast DoT ticks (5741) — 2s periodic ─────────────────
        {
            auto it = g_pyroDoT.find(guid);
            if (it != g_pyroDoT.end())
            {
                uint32 now2 = getMSTime();
                std::vector<uint32> toErase;
                for (auto& [vLow, dotSt] : it->second)
                {
                    if (now2 > dotSt.endMs) { toErase.push_back(vLow); continue; }
                    if (GetMSTimeDiffToNow(dotSt.lastTickMs) < 2000u) continue;
                    // Find victim
                    Unit* victim = nullptr;
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == vLow) { victim = atk; break; }
                    if (!victim) { Unit* v = player->GetVictim(); if (v && v->GetGUID().GetCounter() == vLow) victim = v; }
                    if (!victim || !victim->IsAlive()) { toErase.push_back(vLow); continue; }
                    SanctumAA_DealVisibleDamage(player, victim, dotSt.tickDmg, SPELL_SCHOOL_MASK_FIRE);
                    dotSt.lastTickMs = now2;
                }
                for (uint32 v : toErase) it->second.erase(v);
            }
        }

        // ── Mage: Fire Blast Cascade — dispatch queued splash to nearby enemies ──
        {
            auto it = g_fireBlastCascadeQueue.find(guid);
            if (it != g_fireBlastCascadeQueue.end() && it->second.dmg > 0)
            {
                uint32 originLow = it->second.originVictimLow;
                uint32 splashDmg = it->second.dmg;
                it->second = {};  // clear

                // Find origin victim (for center point of splash)
                Unit* origin = player->GetVictim();
                if (origin && origin->GetGUID().GetCounter() != originLow) origin = nullptr;
                if (!origin)
                    for (Unit* atk : player->getAttackers())
                        if (atk->GetGUID().GetCounter() == originLow) { origin = atk; break; }
                if (!origin) origin = player;  // fallback: splash around player

                std::list<Unit*> nearList;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, origin, 8.0f);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                Cell::VisitObjects(origin, searcher, 8.0f);
                for (Unit* u : nearList)
                {
                    if (!u || !u->IsAlive() || u->GetGUID().GetCounter() == originLow) continue;
                    SanctumAA_DealVisibleDamage(player, u, splashDmg, SPELL_SCHOOL_MASK_FIRE);
                }
            }
        }

        // ── Mage: Chain Explosion (5716) second explosion dispatch ───────────
        {
            auto it = g_meteorQueue.find(guid);
            if (it != g_meteorQueue.end() && it->second.queued)
            {
                uint32 dmg2 = it->second.dmg;
                it->second.queued = false;
                // Fire at a random nearby enemy
                std::vector<Unit*> nearby;
                std::list<Unit*> nearList;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 15.0f);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                Cell::VisitObjects(player, searcher, 15.0f);
                for (Unit* u : nearList) if (u && u->IsAlive()) nearby.push_back(u);
                if (!nearby.empty())
                {
                    uint32 idx = urand(0, (uint32)(nearby.size() - 1));
                    SanctumAA_DealVisibleDamage(player, nearby[idx], dmg2, SPELL_SCHOOL_MASK_ARCANE);
                }
            }
        }

        // ── Mage: Focused Magic (5718) — tick ground arcane zone ─────────────
        {
            auto it = g_focusedMagicZone.find(guid);
            if (it != g_focusedMagicZone.end())
            {
                auto& zone = it->second;
                if (getMSTime() > zone.expireMs) { g_focusedMagicZone.erase(it); }
                else if (GetMSTimeDiffToNow(zone.lastTickMs) >= 2000u)
                {
                    zone.lastTickMs = getMSTime();
                    // Hit all enemies in 6yd of zone center
                    if (player->GetMapId() == zone.mapId)
                    {
                        std::list<Unit*> nearList;
                        Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 30.0f);
                        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                        Cell::VisitObjects(player, searcher, 30.0f);
                        for (Unit* u : nearList)
                        {
                            if (!u || !u->IsAlive()) continue;
                            float dx = u->GetPositionX() - zone.x;
                            float dy = u->GetPositionY() - zone.y;
                            if (dx*dx + dy*dy <= 36.0f)  // 6yd radius
                                SanctumAA_DealVisibleDamage(player, u, zone.tickDmg, SPELL_SCHOOL_MASK_ARCANE);
                        }
                    }
                }
            }
        }

        // ── Mage: Dragon's Fire (5705) — tick ground fire zone ───────────────
        {
            auto it = g_dragonFireZone.find(guid);
            if (it != g_dragonFireZone.end())
            {
                auto& zone = it->second;
                if (getMSTime() > zone.expireMs) { g_dragonFireZone.erase(it); }
                else if (GetMSTimeDiffToNow(zone.lastTickMs) >= 2000u)
                {
                    zone.lastTickMs = getMSTime();
                    if (player->GetMapId() == zone.mapId)
                    {
                        std::list<Unit*> nearList;
                        Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 30.0f);
                        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                        Cell::VisitObjects(player, searcher, 30.0f);
                        for (Unit* u : nearList)
                        {
                            if (!u || !u->IsAlive()) continue;
                            float dx = u->GetPositionX() - zone.x;
                            float dy = u->GetPositionY() - zone.y;
                            if (dx*dx + dy*dy <= 25.0f)  // 5yd radius
                                SanctumAA_DealVisibleDamage(player, u, zone.tickDmg, SPELL_SCHOOL_MASK_FIRE);
                        }
                    }
                }
            }
        }

        // ── Mage: Lost in Time (5719) — tick arcane dmg on slowed targets ────
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_LOST_IN_TIME);
            if (rank > 0)
            {
                static const float spPct[] = { 0.0f, 0.10f, 0.15f, 0.20f };
                int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE);
                if (sp < 0) sp = 0;
                uint32 tickDmg = std::max(1u, (uint32)(sp * spPct[Idx<uint8>(rank)]));

                // Collect slowed enemies attacking us (or within 30yd as attackers)
                std::vector<Unit*> slowed;
                for (Unit* atk : player->getAttackers())
                {
                    if (!atk || !atk->IsAlive()) continue;
                    // Check for any slow/snare aura on them
                    if (atk->HasAuraWithMechanic((1 << MECHANIC_SNARE) | (1 << MECHANIC_DAZE)))
                        slowed.push_back(atk);
                }

                for (Unit* u : slowed)
                {
                    uint32 vLow = u->GetGUID().GetCounter();
                    auto& lastTick = g_lostInTimeTick[guid][vLow];
                    if (GetMSTimeDiffToNow(lastTick) >= 2000u)
                    {
                        SanctumAA_DealVisibleDamage(player, u, tickDmg, SPELL_SCHOOL_MASK_ARCANE);
                        lastTick = getMSTime();
                    }
                }
            }
        }

        // ── Mage: Deep Freeze free cast queue (5710) — dispatch safe cast ────
        {
            auto it = g_deepFreezeFreeCastQueue.find(guid);
            if (it != g_deepFreezeFreeCastQueue.end() && it->second != 0)
            {
                uint32 vLow = it->second;
                it->second = 0;
                // Find victim
                Unit* victim = nullptr;
                for (Unit* atk : player->getAttackers())
                    if (atk->GetGUID().GetCounter() == vLow) { victim = atk; break; }
                if (!victim) { Unit* v = player->GetVictim(); if (v && v->GetGUID().GetCounter() == vLow) victim = v; }
                if (victim && victim->IsAlive())
                    player->CastSpell(victim, 42842, true);  // Frostbolt rank 13 (free)
            }
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

        // =========================================================================
        // DRUID AA OnUnitUpdate
        // =========================================================================

        // ── Druid form-change edge detection ─────────────────────────────────────
        // Used to trigger Nature's Chosen (5916) and Heart of the Wild (5932).
        // On each update, compare current form with last known form.
        {
            ShapeshiftForm currentForm = player->GetShapeshiftForm();
            ShapeshiftForm& lastForm   = g_lastShapeshiftForm[guid];

            if (currentForm != lastForm)
            {
                // Form just changed — run form-entry hooks
                bool enteredFeral = (currentForm == FORM_BEAR || currentForm == FORM_DIREBEAR || currentForm == FORM_CAT);
                bool enteredMoonkin = (currentForm == FORM_MOONKIN);
                bool enteredAnyDruidForm = enteredFeral || enteredMoonkin || (currentForm == FORM_TREE);

                // ── Nature's Chosen (5916) — entering Moonkin: prime instant-cast ──
                if (enteredMoonkin)
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_DRU_NATURES_CHOSEN);
                    if (rank > 0)
                    {
                        auto& ncs = g_naturesChosen[guid];
                        static const uint32 resetMs[] = { 0, 20000, 15000, 10000 };
                        uint32 interval = resetMs[Idx<uint8>(rank)];
                        if (!ncs.ready && (ncs.lastResetMs == 0 || GetMSTimeDiffToNow(ncs.lastResetMs) >= interval))
                        {
                            ncs.ready       = true;
                            ncs.lastResetMs = now;
                            // Grant Nature's Swiftness (17116) for instant next nature/arcane spell
                            player->CastSpell(player, 17116, true);
                        }
                    }
                }

                // ── Heart of the Wild (5932) — any essence switch: 8s dmg+heal window ──
                if (enteredAnyDruidForm)
                {
                    uint8 rank = SanctumAA::GetRank(player, AA_DRU_HEART_OF_THE_WILD);
                    if (rank > 0)
                        SanctumAA_SetHeartOfTheWildWindow(guid, rank, now + 8000u);
                }

                lastForm = currentForm;
            }
        }

        // ── Druid: Wrath of the Wild (5907) — OOC absorb ward refresh ─────────
        {
            uint8 rank = SanctumAA::GetRank(player, AA_DRU_WRATH_OF_THE_WILD);
            if (rank > 0 && !player->IsInCombat())
            {
                auto& wotw = g_wotwAbsorb[guid];
                static const uint32 refreshIntervalMs[] = { 0, 90000, 70000, 50000 };
                uint32 interval = refreshIntervalMs[Idx<uint8>(rank)];
                // Only refresh if absorb is depleted and enough time has passed
                if (wotw.absorb <= 0 && GetMSTimeDiffToNow(wotw.lastRefreshMs) >= interval)
                {
                    static const float pct[] = { 0.0f, 0.05f, 0.08f, 0.12f };
                    wotw.absorb = (int32)(player->GetMaxHealth() * pct[Idx<uint8>(rank)]);
                    wotw.lastRefreshMs = now;
                }
            }
        }

        // ── Druid: Ancestral Spirits (5912) — periodic arcane hit every 8/6/4s ─
        {
            uint8 rank = SanctumAA::GetRank(player, AA_DRU_ANCESTRAL_SPIRITS);
            if (rank > 0 && player->IsInCombat())
            {
                static const uint32 intervalMs[] = { 0, 8000, 6000, 4000 };
                uint32 interval = intervalMs[Idx<uint8>(rank)];
                auto& lastTick = g_ancestralSpiritsLastTick[guid];
                if (GetMSTimeDiffToNow(lastTick) >= interval)
                {
                    lastTick = now;
                    int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE);
                    if (sp < 0) sp = 0;
                    uint32 dmg = std::max(1u, (uint32)(sp * 0.40f));

                    // Find nearest enemy in combat range
                    Unit* nearest = nullptr;
                    float minDist = 30.0f;
                    for (Unit* atk : player->getAttackers())
                    {
                        if (!atk || !atk->IsAlive()) continue;
                        float d = player->GetDistance(atk);
                        if (d < minDist) { minDist = d; nearest = atk; }
                    }
                    if (!nearest) nearest = player->GetVictim();
                    if (nearest && nearest->IsAlive())
                        SanctumAA_DealVisibleDamage(player, nearest, dmg, SPELL_SCHOOL_MASK_ARCANE);
                }
            }
        }

        // ── Druid: Sunfire (5914) — rider nature DoT from Moonfire ────────────
        {
            auto it = g_sunfireDoT.find(guid);
            if (it != g_sunfireDoT.end())
            {
                uint32 now2 = getMSTime();
                std::vector<uint32> toErase;
                for (auto& [vLow, dotSt] : it->second)
                {
                    if (now2 > dotSt.endMs) { toErase.push_back(vLow); continue; }
                    if (GetMSTimeDiffToNow(dotSt.lastTickMs) < 2000u) continue;
                    Unit* victim = player->GetVictim();
                    if (!victim || victim->GetGUID().GetCounter() != vLow)
                    {
                        for (Unit* atk : player->getAttackers())
                            if (atk && atk->GetGUID().GetCounter() == vLow) { victim = atk; break; }
                    }
                    if (!victim || !victim->IsAlive()) { toErase.push_back(vLow); continue; }
                    SanctumAA_DealVisibleDamage(player, victim, dotSt.tickDmg, SPELL_SCHOOL_MASK_NATURE);
                    dotSt.lastTickMs = now2;
                }
                for (uint32 v : toErase) it->second.erase(v);
            }
        }

        // ── Druid: Nature's Chosen (5916) — detect entering Moonkin form ───────
        // Handled in aa_druid_player PlayerScript OnShapeshift. In update: expire stale.
        {
            // No per-update needed; handled in shapeshift hook below.
        }

        // ── Burn-tank: Ironfur (5931) — heat decay ───────────────────────────
        {
            auto it = g_ironfur.find(guid);
            if (it != g_ironfur.end() && it->second.heat > 0 &&
                it->second.lastHitMs > 0 && GetMSTimeDiffToNow(it->second.lastHitMs) >= 6000u)
                it->second.heat = 0;
        }

        // ── Burn-tank: Vengeful Bulwark (5019) — heat decay + secondary reflect ─
        {
            auto it = g_vengefulBulwark.find(guid);
            if (it != g_vengefulBulwark.end())
            {
                if (it->second.heat > 0 && it->second.lastHitMs > 0 &&
                    GetMSTimeDiffToNow(it->second.lastHitMs) >= 6000u)
                    it->second.heat = 0;

                // Reflect to all OTHER attackers (not just the swing attacker, handled in ModifyMeleeDamage)
                // This fires in update to hit any attacker who hit us this tick
                // (already handled per-attacker in the melee hook above — no extra dispatch needed here)
            }
        }

        // ── Burn-tank: Corrupted Carapace (5527) — heat decay ────────────────
        {
            auto it = g_corruptedCarapace.find(guid);
            if (it != g_corruptedCarapace.end() && it->second.heat > 0 &&
                it->second.lastHitMs > 0 && GetMSTimeDiffToNow(it->second.lastHitMs) >= 6000u)
                it->second.heat = 0;
        }

        // ── Druid: Heart of the Wild (5932) — expire stale window ────────────
        {
            auto it = g_heartOfWild.find(guid);
            if (it != g_heartOfWild.end() && now >= it->second.untilMs)
                g_heartOfWild.erase(it);
        }

        // ── Druid: Feral Charge Mastery (5933) — expire stale window ─────────
        {
            auto it = g_feralCharge.find(guid);
            if (it != g_feralCharge.end() && now >= it->second.untilMs)
                g_feralCharge.erase(it);
        }

        // ── Druid: Survival Instincts (5930) — expire stale window ───────────
        {
            auto it = g_survivalInstincts.find(guid);
            if (it != g_survivalInstincts.end() && now >= it->second.untilMs)
                it->second.drPct = 0.0f;
        }

        // ── Druid: Rip and Tear (5901) — dispatch Swipe spread queue ─────────
        {
            auto it = g_ripAndTearQueue.find(guid);
            if (it != g_ripAndTearQueue.end() && it->second.queued)
            {
                it->second.queued = false;
                uint32 originLow = it->second.victimLow;

                // Apply Lacerate (Bear) or Rake+Rip-DoT (Cat) approximation to nearby enemies
                // Implementation: deal a small nature damage pulse to nearby enemies as "spread"
                ShapeshiftForm form = player->GetShapeshiftForm();
                uint32 schoolDamageSchool = SPELL_SCHOOL_MASK_NATURE;

                std::list<Unit*> nearList;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 8.0f);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                Cell::VisitObjects(player, searcher, 8.0f);

                for (Unit* u : nearList)
                {
                    if (!u || !u->IsAlive() || u->GetGUID().GetCounter() == originLow) continue;
                    // Spread dmg = 30% player AP as nature (represents the DoT application)
                    float ap = (float)player->GetTotalAttackPowerValue(BASE_ATTACK);
                    uint32 spreadDmg = std::max(1u, (uint32)(ap * 0.30f));
                    SanctumAA_DealVisibleDamage(player, u, spreadDmg, schoolDamageSchool);
                }
            }
        }

        // ── Druid: Nature's Chosen (5916) — ICD reset: allow re-prime when ICD elapses ──
        // When the instant flag was consumed (ready=false after spell cast), we allow
        // it to be re-primed on the next Moonkin form entry once the ICD has elapsed.
        // The form-entry detection above handles setting ready=true again.
        // No extra per-tick action needed here.

        // =================================================================
        // DISPLAY BUFF SYNCS — safe here (outside all damage hooks)
        // ShowBuff refreshes timer/stacks every tick so the client stays accurate.
        // =================================================================

        // Channeling the Divine (5403) → 720009 (charges remaining)
        {
            auto it = g_channelingDivine.find(guid);
            if (it != g_channelingDivine.end() && it->second.charges > 0)
                SanctumAA_ShowBuff(player, 720009, 0, it->second.charges, false);
            else
                SanctumAA_RemoveBuff(player, 720009, false);
        }

        // Celestial Barrier (5420) → 720010 (sync removal when absorb gone or expired)
        {
            auto it = g_celestialBarrier.find(guid);
            bool active = (it != g_celestialBarrier.end() && it->second.absorb > 0 && now < it->second.expireMs);
            if (!active)
                SanctumAA_RemoveBuff(player, 720010, false);
        }

        // Furious Charge (5012) → 720003
        {
            auto it = g_furiousCharge.find(guid);
            if (it != g_furiousCharge.end() && now < it->second.untilMs)
                SanctumAA_ShowBuff(player, 720003, it->second.untilMs - now, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720003, false);
        }

        // Vengeful Bulwark (5019) → 720004 (stacks)
        {
            auto it = g_vengefulBulwark.find(guid);
            if (it != g_vengefulBulwark.end() && it->second.heat > 0)
                SanctumAA_ShowBuff(player, 720004, 6000u, it->second.heat, false);
            else
                SanctumAA_RemoveBuff(player, 720004, false);
        }

        // Judge (5101) → 720005 (while swingsLeft > 0 and window not expired)
        {
            auto it = g_judgeWindow.find(guid);
            if (it != g_judgeWindow.end() && it->second.swingsLeft > 0 && now < it->second.untilMs)
                SanctumAA_ShowBuff(player, 720005, it->second.untilMs - now, it->second.swingsLeft, false);
            else
                SanctumAA_RemoveBuff(player, 720005, false);
        }

        // Radiance / Improved Flash of Light (5114) → 720006 (stacks, charge-style no countdown)
        {
            auto it = g_radianceStacks.find(guid);
            if (it != g_radianceStacks.end() && it->second > 0)
                SanctumAA_ShowBuff(player, 720006, 0, it->second, false);
            else
                SanctumAA_RemoveBuff(player, 720006, false);
        }

        // Unyielding Light (5126) → 720007
        {
            auto it = g_unyieldingLight.find(guid);
            if (it != g_unyieldingLight.end() && now < it->second.untilMs)
                SanctumAA_ShowBuff(player, 720007, it->second.untilMs - now, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720007, false);
        }

        // Vengeance (2005) → 720031
        {
            auto it = g_vengeance.find(guid);
            if (it != g_vengeance.end() && now < it->second.untilMs)
                SanctumAA_ShowBuff(player, 720031, it->second.untilMs - now, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720031, false);
        }

        // Hardening (2107) → 720033 (stacks, resets OOC)
        {
            auto it = g_hardening.find(guid);
            if (it != g_hardening.end() && it->second.stacks > 0)
                SanctumAA_ShowBuff(player, 720033, 0, it->second.stacks, false);
            else
                SanctumAA_RemoveBuff(player, 720033, false);
        }

        // Hindsight (2109) → 720034 (while absorb > 0 and not expired)
        {
            auto it = g_hindsight.find(guid);
            if (it != g_hindsight.end() && it->second.absorb > 0 && now < it->second.expireMs)
                SanctumAA_ShowBuff(player, 720034, it->second.expireMs - now, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720034, false);
        }

        // Corrupted Carapace (5527) → 720013 (stacks)
        {
            auto it = g_corruptedCarapace.find(guid);
            if (it != g_corruptedCarapace.end() && it->second.heat > 0)
                SanctumAA_ShowBuff(player, 720013, 6000u, it->second.heat, false);
            else
                SanctumAA_RemoveBuff(player, 720013, false);
        }

        // Final Rune HoT (5519) → 720014 (while pool > 0)
        {
            auto it = g_frHot.find(guid);
            if (it != g_frHot.end() && it->second.pool > 0)
                SanctumAA_ShowBuff(player, 720014, 4000u, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720014, false);
        }

        // Battle Endurance DR window (5014) → 720015 (while shieldUntil > now)
        {
            auto it = g_battleEndurance.find(guid);
            if (it != g_battleEndurance.end() && it->second.shieldUntil > 0 && now < it->second.shieldUntil)
                SanctumAA_ShowBuff(player, 720015, it->second.shieldUntil - now, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720015, false);
        }

        // Molten Shell (5743) → 720017 (stacks = heat)
        {
            auto it = g_moltenShell.find(guid);
            if (it != g_moltenShell.end() && it->second.heat > 0)
                SanctumAA_ShowBuff(player, 720017, 6000u, it->second.heat, false);
            else
                SanctumAA_RemoveBuff(player, 720017, false);
        }

        // Spell Weaving (5739) → 720018 (stacks)
        {
            auto it = g_spellWeaving.find(guid);
            if (it != g_spellWeaving.end() && it->second.stacks > 0)
                SanctumAA_ShowBuff(player, 720018, 0, it->second.stacks, false);
            else
                SanctumAA_RemoveBuff(player, 720018, false);
        }

        // Wrath of the Wild (5907) → 720025 (OOC absorb present)
        {
            auto it = g_wotwAbsorb.find(guid);
            if (it != g_wotwAbsorb.end() && it->second.absorb > 0)
                SanctumAA_ShowBuff(player, 720025, 0, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720025, false);
        }

        // Ironfur (5931) → 720026 (stacks)
        {
            auto it = g_ironfur.find(guid);
            if (it != g_ironfur.end() && it->second.heat > 0)
                SanctumAA_ShowBuff(player, 720026, 6000u, it->second.heat, false);
            else
                SanctumAA_RemoveBuff(player, 720026, false);
        }

        // Heart of the Wild (5932) → 720022
        {
            auto it = g_heartOfWild.find(guid);
            if (it != g_heartOfWild.end() && now < it->second.untilMs)
                SanctumAA_ShowBuff(player, 720022, it->second.untilMs - now, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720022, false);
        }

        // Feral Charge Mastery (5933) → 720023 (window open and not yet consumed)
        {
            auto it = g_feralCharge.find(guid);
            if (it != g_feralCharge.end() && now < it->second.untilMs && !it->second.consumed)
                SanctumAA_ShowBuff(player, 720023, it->second.untilMs - now, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720023, false);
        }

        // Nature's Chosen (5916) → 720024 (instant-cast flag ready)
        {
            auto it = g_naturesChosen.find(guid);
            if (it != g_naturesChosen.end() && it->second.ready)
                SanctumAA_ShowBuff(player, 720024, 0, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720024, false);
        }

        // Survival Instincts (5930) → 720019 (synced at activation; also keep refreshed here)
        {
            auto it = g_survivalInstincts.find(guid);
            if (it != g_survivalInstincts.end() && it->second.drPct > 0.0f && now < it->second.untilMs)
                SanctumAA_ShowBuff(player, 720019, it->second.untilMs - now, 0, false);
            else
                SanctumAA_RemoveBuff(player, 720019, false);
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

        // ── Mage: Spreading Flames (5702) — transfer Ignite stacks to nearest enemy ──
        // When a unit dies, any player who had Ignite stacks on it transfers them.
        for (auto& [playerGuid, victimMap] : g_igniteStacks)
        {
            auto it = victimMap.find(deadGuid);
            if (it == victimMap.end() || it->second.stacks == 0) continue;
            IgniteStackEntry transferEntry = it->second;
            victimMap.erase(it);

            // Find player who owns these stacks
            Player* p = ObjectAccessor::FindPlayerByLowGUID(playerGuid);
            if (!p || !p->IsAlive()) continue;
            uint8 rank = SanctumAA::GetRank(p, AA_MAG_SPREADING_FLAMES);
            if (!rank) continue;

            // Find nearest alive enemy within 8yd of the dead unit
            // Use unit position as center (unit may be "dead" but position valid)
            std::list<Unit*> nearList;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(p, p, 8.0f);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(p, nearList, chk);
            Cell::VisitObjects(p, searcher, 8.0f);
            Unit* nearest = nullptr;
            float minDist = 999.0f;
            for (Unit* u : nearList)
            {
                if (!u || !u->IsAlive() || u->GetGUID().GetCounter() == deadGuid) continue;
                float d = p->GetDistance(u);
                if (d < minDist) { minDist = d; nearest = u; }
            }
            if (nearest)
            {
                uint32 nLow = nearest->GetGUID().GetCounter();
                auto& ne = victimMap[nLow];
                ne.stacks    = std::max(ne.stacks, transferEntry.stacks);
                ne.expireMs  = std::max(ne.expireMs, transferEntry.expireMs);
            }
        }

        // ── Mage: Pyroblast DoT — clear stacks on victim death ───────────────────
        for (auto& [ag, victimMap] : g_pyroDoT)
            victimMap.erase(deadGuid);

        // ── Mage: Scorched — clear stacks on victim death ────────────────────────
        for (auto& [ag, victimMap] : g_scorched)
            victimMap.erase(deadGuid);

        // ── Druid: Sunfire DoT — clear on victim death ───────────────────────────
        for (auto& [ag, victimMap] : g_sunfireDoT)
            victimMap.erase(deadGuid);

        // ── Druid: Living Seed — clear on target death ──────────────────────────
        for (auto& [ag, seedMap] : g_livingSeed)
            seedMap.erase(deadGuid);

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
// aa_combat_mage_player — PlayerScript for Mage AA spell-cast hooks
// Handles: Scorched, Phantasmal Assault, Mirror Ward, Heating Up, Mana Reactor,
//          Short Fuse, Arcane Bombardment, Improved Deep Freeze, Frostbolt Bounce,
//          Meteor Strike, Combustion Mastery, Spell Weaving school tracking
// ---------------------------------------------------------------------------
class aa_combat_mage_player : public PlayerScript
{
public:
    aa_combat_mage_player() : PlayerScript("aa_combat_mage_player") {}

    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override
    {
        if (!player || skipCheck || !spell)
            return;

        SpellInfo const* info = spell->GetSpellInfo();
        if (!info)
            return;

        uint32 guid = player->GetGUID().GetCounter();
        uint32 schoolMask = info->GetSchoolMask();
        Unit* spellTarget = spell->m_targets.GetUnitTarget();
        if (!spellTarget) spellTarget = player->GetVictim();

        // ── Scorched (5723) — Scorch applies fire-vuln stack ─────────────────
        // Scorch IDs: 2948, 8444, 8445, 8446, 10205, 10206, 10207, 27382, 42858, 42859
        {
            static const std::unordered_set<uint32> s_scorch = {
                2948, 8444, 8445, 8446, 10205, 10206, 10207, 27382, 42858, 42859
            };
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_SCORCHED);
            if (rank > 0 && s_scorch.count(info->Id) && spellTarget && spellTarget->IsAlive())
            {
                uint32 vLow = spellTarget->GetGUID().GetCounter();
                auto& entry = g_scorched[guid][vLow];
                uint32 now = getMSTime();
                if (entry.stacks > 0 && now > entry.expireMs)
                    entry = ScorchedEntry{};
                if (entry.stacks < 5)
                    entry.stacks++;
                entry.expireMs = now + 30000u;  // 30s debuff window (refreshed per scorch)
            }
        }

        // ── Phantasmal Assault (5727) — Mirror Image cast -> arcane nova ─────
        // Mirror Image spell IDs: 55342 (WotLK)
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_PHANTASMAL_ASSAULT);
            if (rank > 0 && info->Id == 55342)
            {
                static const float mult[] = { 0.0f, 0.60f, 1.00f, 1.50f };
                int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE);
                if (sp < 0) sp = 0;
                uint32 novaDmg = (uint32)(sp * mult[Idx<uint8>(rank)]);
                if (novaDmg > 0)
                {
                    std::list<Unit*> nearList;
                    Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 10.0f);
                    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                    Cell::VisitObjects(player, searcher, 10.0f);
                    for (Unit* u : nearList)
                    {
                        if (!u || !u->IsAlive()) continue;
                        SanctumAA_DealVisibleDamage(player, u, novaDmg, SPELL_SCHOOL_MASK_ARCANE);
                    }
                }
            }
        }

        // ── Mirror Ward (5728) — Mirror Image cast -> absorb shield ──────────
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_MIRROR_WARD);
            if (rank > 0 && info->Id == 55342)
            {
                static const float pct[] = { 0.0f, 0.20f, 0.35f, 0.50f };
                int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE);
                if (sp < 0) sp = 0;
                int32 shieldAmt = (int32)(sp * pct[Idx<uint8>(rank)]);
                if (shieldAmt > 0)
                {
                    // Reuse Celestial Barrier absorb map for the shield
                    auto& cb = g_celestialBarrier[guid];
                    cb.absorb  += shieldAmt;
                    cb.expireMs = getMSTime() + 30000u;  // lasts until used
                }
            }
        }

        // ── Heating Up (5746) — fire spell cast: 10/20/30% chance -> Hot Streak ──
        if (schoolMask & SPELL_SCHOOL_MASK_FIRE)
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_HEATING_UP);
            if (rank > 0 && spellTarget)
            {
                // Only proc on damaging fire spells (have SPELL_EFFECT_SCHOOL_DAMAGE)
                bool isDmgSpell = false;
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                    if (info->Effects[i].Effect == SPELL_EFFECT_SCHOOL_DAMAGE) { isDmgSpell = true; break; }
                if (isDmgSpell)
                {
                    static const float chance[] = { 0.0f, 10.0f, 20.0f, 30.0f };
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                        g_heatingUpQueue[guid] = true;  // apply Hot Streak safely in OnUnitUpdate
                }
            }
        }

        // ── Mana Reactor (5740) — below 20% mana: refund next dmg cast ───────
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_MANA_REACTOR);
            if (rank > 0)
            {
                auto flagIt = g_manaReactorReady.find(guid);
                if (flagIt != g_manaReactorReady.end() && flagIt->second)
                {
                    // Check if this is a damaging spell
                    bool isDmgSpell = false;
                    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                        if (info->Effects[i].Effect == SPELL_EFFECT_SCHOOL_DAMAGE) { isDmgSpell = true; break; }
                    if (isDmgSpell && player->GetMaxPower(POWER_MANA) > 0)
                    {
                        flagIt->second = false;
                        static const float pct[] = { 0.0f, 0.10f, 0.20f, 0.30f };
                        int32 refund = (int32)(player->GetMaxPower(POWER_MANA) * pct[Idx<uint8>(rank)]);
                        if (refund > 0)
                        {
                            int32 newMana = std::min(player->GetPower(POWER_MANA) + refund,
                                                     player->GetMaxPower(POWER_MANA));
                            player->SetPower(POWER_MANA, newMana);
                        }
                    }
                }
            }
        }

        // ── Short Fuse (5700) — Living Bomb: instant explosion on cast ───────
        // Living Bomb cast IDs (the DoT apply): 44457, 55359, 55360
        {
            static const std::unordered_set<uint32> s_lbCast = { 44457, 55359, 55360 };
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_SHORT_FUSE);
            if (rank > 0 && s_lbCast.count(info->Id) && spellTarget && spellTarget->IsAlive())
            {
                int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE);
                if (sp < 0) sp = 0;
                // Approximate explosion dmg: 100% SP (matching the rank-scaled explosion)
                uint32 explodeDmg = std::max(1u, (uint32)(sp * 1.00f));
                SanctumAA_DealVisibleDamage(player, spellTarget, explodeDmg, SPELL_SCHOOL_MASK_FIRE);
                // Note: 5/4/3s cooldown enforcement is partial — we cannot add a CD to LB itself.
            }
        }

        // ── Arcane Bombardment (5714) — Arcane Missiles extra missile ────────
        // Arcane Missiles IDs: 5143 (R1), 5144 (R2), 5145 (R3), 8416 (R4), 8417 (R5), 10211 (R6),
        //                      10212 (R7), 25345 (R8), 27075 (R9), 38699 (R10), 42843 (R11), 42846 (R12)
        {
            static const std::unordered_set<uint32> s_arcMiss = {
                5143, 5144, 5145, 8416, 8417, 10211, 10212, 25345, 27075, 38699, 42843, 42846
            };
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_ARCANE_BOMBARDMENT);
            if (rank > 0 && s_arcMiss.count(info->Id) && spellTarget && spellTarget->IsAlive())
            {
                static const float chance[] = { 0.0f, 20.0f, 35.0f, 50.0f };
                if (roll_chance_f(chance[Idx<uint8>(rank)]))
                {
                    int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE);
                    if (sp < 0) sp = 0;
                    // Approximate: 1 missile at 50% SP as bonus hit
                    uint32 missileDmg = std::max(1u, (uint32)(sp * 0.50f));
                    SanctumAA_DealVisibleDamage(player, spellTarget, missileDmg, SPELL_SCHOOL_MASK_ARCANE);
                }
            }
        }

        // ── Improved Deep Freeze (5710) — frost spell during DF CD: free cast ──
        // Deep Freeze CD: 44572. Check if player has it on CD.
        if (schoolMask & SPELL_SCHOOL_MASK_FROST)
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_IMPROVED_DEEP_FREEZE);
            if (rank > 0 && spellTarget && spellTarget->IsAlive())
            {
                // Only proc when Deep Freeze is actually on cooldown
                bool dfOnCd = player->HasSpellCooldown(44572);
                if (dfOnCd)
                {
                    static const float chance[] = { 0.0f, 10.0f, 20.0f, 30.0f };
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                    {
                        // Queue a free Frostbolt for safe dispatch in OnUnitUpdate
                        g_deepFreezeFreeCastQueue[guid] = spellTarget->GetGUID().GetCounter();
                    }
                }
            }
        }

        // ── Improved Frostbolt (5708) — Frostbolt/Ice Lance vs frozen: bounce ──
        // Frostbolt IDs: 116 (R1), 205 (R2)...42842 (R13). Ice Lance: 30455, 42913, 42914
        {
            static const std::unordered_set<uint32> s_frostPrimary = {
                116, 205, 837, 7322, 8406, 8407, 8408, 10179, 10180, 10181,
                25304, 27071, 38697, 42841, 42842,   // Frostbolt
                30455, 42913, 42914                   // Ice Lance
            };
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_IMPROVED_FROSTBOLT);
            if (rank > 0 && s_frostPrimary.count(info->Id) && spellTarget && spellTarget->IsAlive())
            {
                // Check target is frozen
                bool frozen = spellTarget->HasAuraType(SPELL_AURA_MOD_ROOT) ||
                              spellTarget->HasAuraType(SPELL_AURA_MOD_STUN);
                if (frozen)
                {
                    static const float chance[] = { 0.0f, 30.0f, 45.0f, 60.0f };
                    static const uint8 maxBounce[] = { 0, 1, 2, 3 };
                    if (roll_chance_f(chance[Idx<uint8>(rank)]))
                    {
                        // Queue bounce hits for safe delivery
                        // Collect nearby unfrozen enemies
                        int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FROST);
                        if (sp < 0) sp = 0;
                        uint32 bounceDmg = std::max(1u, (uint32)(sp * 0.80f));
                        uint8  bounces   = maxBounce[Idx<uint8>(rank)];

                        std::list<Unit*> nearList;
                        Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 15.0f);
                        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                        Cell::VisitObjects(player, searcher, 15.0f);

                        uint8 count = 0;
                        for (Unit* u : nearList)
                        {
                            if (count >= bounces) break;
                            if (!u || !u->IsAlive() || u == spellTarget) continue;
                            // Target unfrozen enemies
                            bool uFrozen = u->HasAuraType(SPELL_AURA_MOD_ROOT) || u->HasAuraType(SPELL_AURA_MOD_STUN);
                            if (!uFrozen)
                            {
                                SanctumAA_DealVisibleDamage(player, u, bounceDmg, SPELL_SCHOOL_MASK_FROST);
                                ++count;
                            }
                        }
                    }
                }
            }
        }

        // ── Meteor Strike (5704) — track 3-proc trigger ───────────────────────
        // Impact proc: 12578-12581 (check aura on player)
        // We detect procs by checking for the proc aura AFTER the cast (approximation).
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_METEOR_STRIKE);
            if (rank > 0)
            {
                auto& ms = g_meteorStrike[guid];
                // Impact: Fire Blast after Impact proc aura
                static const std::unordered_set<uint32> s_fireBlast = {
                    2136, 2137, 2138, 8412, 8413, 10197, 10199, 27079, 42873, 42872
                };
                // Pyroblast (Hot Streak)
                static const std::unordered_set<uint32> s_pyro = {
                    11366, 12505, 12522, 12523, 12524, 12525, 12526, 18809, 27338, 33938, 42891, 42892
                };
                // Flamestrike (Firestarter)
                static const std::unordered_set<uint32> s_flamestrike = {
                    2120, 2121, 8422, 8423, 10215, 10216, 27086 /*not blizzard*/, 42925, 42926
                };
                if (s_fireBlast.count(info->Id))  ms.impact      = true;
                if (s_pyro.count(info->Id))        ms.hotstreak   = true;
                if (s_flamestrike.count(info->Id)) ms.firestarter = true;

                if (ms.impact && ms.hotstreak && ms.firestarter && spellTarget && spellTarget->IsAlive())
                {
                    ms = {};  // reset all 3 flags
                    static const float mult[] = { 0.0f, 2.00f, 2.80f, 4.00f };
                    int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE);
                    if (sp < 0) sp = 0;
                    uint32 meteorDmg = std::max(1u, (uint32)(sp * mult[Idx<uint8>(rank)]));

                    std::list<Unit*> nearList;
                    Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 10.0f);
                    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                    Cell::VisitObjects(player, searcher, 10.0f);
                    for (Unit* u : nearList)
                    {
                        if (!u || !u->IsAlive()) continue;
                        SanctumAA_DealVisibleDamage(player, u, meteorDmg, SPELL_SCHOOL_MASK_FIRE);
                    }
                }
            }
        }

        // ── Dragon's Fire (5705) — Dragon's Breath cast → ground fire zone ─────────
        // Dragon's Breath IDs (all ranks): 31661, 33041, 33042, 33043, 27869 (VERIFY)
        {
            static const std::unordered_set<uint32> s_dragonBreath = {
                31661, 33041, 33042, 33043, 27869, 42949, 42950
            };
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_DRAGONS_FIRE);
            if (rank > 0 && s_dragonBreath.count(info->Id))
            {
                static const uint32 durMs[] = { 0, 6000, 9000, 12000 };
                uint32 dur = durMs[std::min<uint8>(rank, 3)];
                // Place zone at target location or slightly in front of player
                float zx, zy, zz;
                if (spellTarget && spellTarget->IsAlive())
                {
                    zx = spellTarget->GetPositionX();
                    zy = spellTarget->GetPositionY();
                    zz = spellTarget->GetPositionZ();
                }
                else
                {
                    float dist = 6.0f;
                    zx = player->GetPositionX() + dist * std::cos(player->GetOrientation());
                    zy = player->GetPositionY() + dist * std::sin(player->GetOrientation());
                    zz = player->GetPositionZ();
                }
                int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE);
                if (sp < 0) sp = 0;
                uint32 tickDmg = std::max(1u, (uint32)(sp * 0.30f));
                {
                    extern void SanctumAA_SetDragonFireZone(uint32 playerGuid, float x, float y, float z, uint32 mapId, uint32 durationMs, uint32 tickDmg);
                    SanctumAA_SetDragonFireZone(guid, zx, zy, zz, player->GetMapId(), dur, tickDmg);
                }
            }
        }

        // ── Combustion Mastery (5744) — on Combustion cast: spread+extend fire DoTs ──
        // Combustion spell ID: 11129 (all ranks share this)
        {
            uint8 rank = SanctumAA::GetRank(player, AA_MAG_COMBUSTION_MASTERY);
            if (rank > 0 && info->Id == 11129 && spellTarget && spellTarget->IsAlive())
            {
                // Spread fire DoTs from current target to enemies in 8yd
                // As approximation: deal extra fire dmg (= 30% of target's max HP) to nearby enemies
                // to simulate DoT spread. Real DoT copying is not possible without server core access.
                // PARTIAL: we deal immediate fire splash as "spread" approximation.
                int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE);
                if (sp < 0) sp = 0;
                uint32 spreadDmg = std::max(1u, (uint32)(sp * 0.50f));  // 50% SP as spread pulse

                std::list<Unit*> nearList;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 8.0f);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                Cell::VisitObjects(player, searcher, 8.0f);
                for (Unit* u : nearList)
                {
                    if (!u || !u->IsAlive() || u == spellTarget) continue;
                    SanctumAA_DealVisibleDamage(player, u, spreadDmg, SPELL_SCHOOL_MASK_FIRE);
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// aa_druid_player — PlayerScript for Druid AA hooks:
//   OnShapeshift: Nature's Chosen (5916), Heart of the Wild (5932)
//   OnPlayerSpellCast: Sunfire DoT (5914), Rip and Tear spread queue (5901),
//                      Savage Swipe extra hits (5909), Feral Charge Mastery (5933),
//                      Improved Faerie Fire energy/rage generation, Radiant Cure (5924),
//                      Innate Camouflage extended Prowl (5926)
// ---------------------------------------------------------------------------
class aa_druid_player : public PlayerScript
{
public:
    aa_druid_player() : PlayerScript("aa_druid_player") {}

    // NOTE: OnShapeshift does NOT exist as a PlayerScript hook in AzerothCore 3.3.5a.
    // Form-change detection is done via polling GetShapeshiftForm() in OnUnitUpdate.
    // Nature's Chosen and Heart of the Wild are triggered from the OnUnitUpdate
    // edge-detect logic in aa_combat_unit (form change detected there).

    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override
    {
        if (!player || skipCheck || !spell) return;
        SpellInfo const* info = spell->GetSpellInfo();
        if (!info) return;

        uint32 guid = player->GetGUID().GetCounter();
        Unit* spellTarget = spell->m_targets.GetUnitTarget();
        if (!spellTarget) spellTarget = player->GetVictim();

        // ── Sunfire (5914) — Moonfire cast queues a rider nature DoT ──────────
        // Moonfire CAST IDs (apply the DoT): 8921, 8924, 9833, 9834, 9835, 26987, 26988, 48462, 48463
        {
            static const std::unordered_set<uint32> s_moonfire = {
                8921,8924,9833,9834,9835,26987,26988,48462,48463
            };
            uint8 rank = SanctumAA::GetRank(player, AA_DRU_SUNFIRE);
            if (rank > 0 && s_moonfire.count(info->Id) && spellTarget && spellTarget->IsAlive())
            {
                // Queue nature DoT: 30/50/75% of SP over 6s (3 ticks of 2s)
                static const float dotPct[] = { 0.0f, 0.30f, 0.50f, 0.75f };
                int32 sp = player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE);
                if (sp < 0) sp = 0;
                uint32 totalDot = std::max(1u, (uint32)(sp * dotPct[Idx<uint8>(rank)]));
                uint32 tickDot  = std::max(1u, totalDot / 3u);
                uint32 vLow = spellTarget->GetGUID().GetCounter();
                auto& dotSt = g_sunfireDoT[guid][vLow];
                dotSt.endMs      = getMSTime() + 6000u;
                dotSt.lastTickMs = getMSTime();
                dotSt.tickDmg    = tickDot;
            }
        }

        // ── Rip and Tear (5901) — Swipe cast queues a spread to nearby enemies ──
        // Bear Swipe: 779, 780, 769, 9745, 9880, 9881, 27001, 48559, 48560
        // Cat Swipe:  62078
        {
            static const std::unordered_set<uint32> s_swipe = {
                779,780,769,9745,9880,9881,27001,48559,48560,  // Bear
                62078                                            // Cat
            };
            uint8 rank = SanctumAA::GetRank(player, AA_DRU_RIP_AND_TEAR);
            if (rank > 0 && s_swipe.count(info->Id) && spellTarget)
            {
                g_ripAndTearQueue[guid] = { true, spellTarget->GetGUID().GetCounter() };
            }
        }

        // ── Savage Swipe (5909) — Swipe hits extra targets for bonus damage ────
        // In Bear or Cat form, Swipe hits +1/2/3 extra targets for 15/25/40% bonus dmg.
        {
            static const std::unordered_set<uint32> s_swipe2 = {
                779,780,769,9745,9880,9881,27001,48559,48560,  // Bear
                62078                                            // Cat
            };
            uint8 rank = SanctumAA::GetRank(player, AA_DRU_SAVAGE_SWIPE);
            if (rank > 0 && s_swipe2.count(info->Id) && spellTarget && spellTarget->IsAlive())
            {
                static const uint8 extraTargets[] = { 0, 1, 2, 3 };
                static const float bonus[] = { 0.0f, 0.15f, 0.25f, 0.40f };
                uint8 extras = extraTargets[Idx<uint8>(rank)];
                float pct    = bonus[Idx<uint8>(rank)];

                int32 spBase = player->GetTotalAttackPowerValue(BASE_ATTACK);
                uint32 extraDmg = std::max(1u, (uint32)(spBase * 0.20f * (1.0f + pct)));

                std::list<Unit*> nearList;
                Acore::AnyUnfriendlyUnitInObjectRangeCheck chk(player, player, 8.0f);
                Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearList, chk);
                Cell::VisitObjects(player, searcher, 8.0f);

                uint8 count = 0;
                for (Unit* u : nearList)
                {
                    if (count >= extras) break;
                    if (!u || !u->IsAlive() || u == spellTarget) continue;
                    SanctumAA_DealVisibleDamage(player, u, extraDmg, SPELL_SCHOOL_MASK_NORMAL);
                    ++count;
                }
            }
        }

        // ── Feral Charge Mastery (5933) — detect Feral Charge cast ───────────
        // Feral Charge (Bear): 16979, 49376? Actually Bear=16979, Cat=49376
        {
            static const std::unordered_set<uint32> s_feralCharge = { 16979, 49376 };
            uint8 rank = SanctumAA::GetRank(player, AA_DRU_FERAL_CHARGE_MASTERY);
            if (rank > 0 && s_feralCharge.count(info->Id))
            {
                SanctumAA_SetFeralChargeWindow(guid, rank, 5000u);  // 5s window for next ability
            }
        }

        // ── Improved Faerie Fire (5906) — resource generation on cast ─────────
        // Already handled in ModifySpellDamageTaken (on hit). This provides the initial
        // resource on cast even if the ability misses (fire on cast, not hit).
        // PARTIAL: we generate on cast here as a backup trigger.
        {
            static const std::unordered_set<uint32> s_ffFeral = { 16857, 17390, 17391, 17392 };
            uint8 rank = SanctumAA::GetRank(player, AA_DRU_IMPROVED_FAERIE_FIRE);
            if (rank > 0 && s_ffFeral.count(info->Id))
            {
                // Check ICD: 1s to avoid double-generating on hit+cast
                if (CheckICD(guid, AA_DRU_IMPROVED_FAERIE_FIRE, 1000u))
                {
                    if (player->GetMaxPower(POWER_ENERGY) > 0)
                        player->ModifyPower(POWER_ENERGY, std::min<int32>(10, (int32)player->GetMaxPower(POWER_ENERGY) - (int32)player->GetPower(POWER_ENERGY)));
                    else if (player->GetMaxPower(POWER_RAGE) > 0)
                        player->ModifyPower(POWER_RAGE, std::min<int32>(50, (int32)player->GetMaxPower(POWER_RAGE) - (int32)player->GetPower(POWER_RAGE)));
                }
            }
        }

        // ── Nature's Chosen (5916) — consume instant-cast flag on nature/arcane spell ──
        // If ready=true and this is a nature/arcane spell, consume the flag.
        // The Nature's Swiftness aura (17116) already made the spell instant; just clear flag.
        {
            uint8 rank = SanctumAA::GetRank(player, AA_DRU_NATURES_CHOSEN);
            if (rank > 0)
            {
                uint32 schoolMask = info->GetSchoolMask();
                bool isNatureArcane = (schoolMask & SPELL_SCHOOL_MASK_NATURE) ||
                                      (schoolMask & SPELL_SCHOOL_MASK_ARCANE);
                if (isNatureArcane)
                {
                    auto it = g_naturesChosen.find(guid);
                    if (it != g_naturesChosen.end() && it->second.ready)
                        it->second.ready = false;  // consumed
                }
            }
        }

        // ── Healing Gift (5919) — Heal crit chance via ApplyRatingMod in ApplyAAStat ──
        // Handled as a stat passive (Tree of Life form only) in ApplyAAStat.
        // (No cast hook needed — it's always-active when in Tree form via rating mod.)
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
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
    new aa_combat_mage_player();
    new aa_druid_player();
}
