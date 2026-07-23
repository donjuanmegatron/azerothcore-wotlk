// mod-lifesteal.cpp
// -------------------------------------------------------------------------
// Sanctum universal lifesteal (Phase 1).
//
// Every character heals for a % of ALL damage they deal — melee, ranged,
// SPELL, and periodic (DoT) — because it hooks the single unified damage point
// (UnitScript::OnDamage, fired at the top of Unit::DealDamage for every damage
// instance). So spell lifesteal is automatic: there is no separate "spell
// leech" mechanic, lifesteal simply applies to every school of damage you deal.
//
// Design (CLAUDE.md "Lifesteal"): 5% flat baseline for ALL characters (no item,
// no opt-in). Later sources stack ON TOP via the exported API below:
//   - Void power stone: +1-5% per tier   (mod-power-stones, once Void is built)
//   - Vital Hunger AA:   +5% per rank     (mod-aa-system)
//   - Pet lifesteal:     pet dmg heals owner (mod-pet-systems)
//
// "Crits double lifesteal" (2026-07-22 "The On-Crit Line") — OnDamage still
// carries no crit flag (Unit::DealDamage/DealSpellDamage call it before the
// engine's own crit-notification hooks fire), so the doubling can't happen
// INSIDE OnDamage itself. Instead: OnDamage always applies the base leech and
// remembers the amount per player; the real-crit signal (melee/ranged via
// PlayerScript::OnPlayerCanCastItemCombatSpell procEx, spell via the new
// UnitScript::OnUnitSpellCrit hook — both fire synchronously, later in the
// SAME damage-resolution call chain, no tick delay) then applies ONE matching
// extra heal to double the total. Pure hook-safe HP restores, no CastSpell/AddAura.
// -------------------------------------------------------------------------

#include "ScriptMgr.h"
#include "Player.h"
#include "Unit.h"
#include "Item.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Log.h"
#include "Timer.h"
#include <unordered_map>

// Baseline lifesteal every character gets, in percent.
static constexpr float LIFESTEAL_BASELINE_PCT = 5.0f;

// Per-player ADDITIONAL lifesteal % contributed by other systems (Void stone /
// Vital Hunger AA / pet AA). Keyed by low GUID. Total% = baseline + this.
static std::unordered_map<uint32, float> g_lifestealBonusPct;

// ---------------------------------------------------------------------------
// Public API — external linkage so other modules can wire in their own
// lifesteal contributions (same extern-declare pattern mod-power-stones uses
// for Shards_*). A source computes its total contribution and calls SetBonus;
// passing 0 clears it. Keep it simple: one flat bonus slot per player that the
// caller keeps up to date (e.g. from a socket/AA recompute).
// ---------------------------------------------------------------------------
void Lifesteal_SetBonusPct(uint32 lowGuid, float pct)
{
    if (pct <= 0.0f)
        g_lifestealBonusPct.erase(lowGuid);
    else
        g_lifestealBonusPct[lowGuid] = pct;
}

float Lifesteal_GetTotalPct(Player* player)
{
    if (!player)
        return 0.0f;
    float bonus = 0.0f;
    auto it = g_lifestealBonusPct.find(player->GetGUID().GetCounter());
    if (it != g_lifestealBonusPct.end())
        bonus = it->second;
    return LIFESTEAL_BASELINE_PCT + bonus;
}

// SANCTUM on-crit — pending-heal bookkeeping for "crits double lifesteal".
// OnDamage stashes the heal amount it just applied here; the real-crit signal
// (arriving synchronously, later in the same damage-resolution chain) consumes
// it and applies one matching extra heal. Timestamped so a crit notification
// can never reach across into an unrelated later hit.
struct PendingLifestealHeal { int32 heal = 0; uint32 atMs = 0; };
static std::unordered_map<uint32, PendingLifestealHeal> g_pendingCritHeal;

static void Lifesteal_NotifyRealCrit(Player* player)
{
    if (!player)
        return;

    uint32 guid = player->GetGUID().GetCounter();
    auto it = g_pendingCritHeal.find(guid);
    if (it == g_pendingCritHeal.end())
        return;

    // Only honor it if it's from THIS hit (well within one tick — real crit
    // notifications fire synchronously, not deferred).
    if (GetMSTimeDiffToNow(it->second.atMs) <= 200 && it->second.heal > 0 && player->IsAlive())
        player->ModifyHealth(it->second.heal); // doubles the base leech to 2x total

    g_pendingCritHeal.erase(it);
}

// ---------------------------------------------------------------------------
// The lifesteal hook
// ---------------------------------------------------------------------------
class LifestealUnitScript : public UnitScript
{
public:
    LifestealUnitScript() : UnitScript("LifestealUnitScript") {}

    // Fires once for EVERY damage instance in Unit::DealDamage (melee, ranged,
    // spell, periodic) — so this single hook covers all damage the player deals,
    // including spell damage.
    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!attacker || !victim || damage == 0 || attacker == victim)
            return;

        Player* player = attacker->ToPlayer();
        if (!player || !player->IsAlive())
            return;

        // Only leech from real combat damage against an attackable enemy (never
        // from friendly-fire edge cases, environmental self-damage, etc.).
        if (!player->IsValidAttackTarget(victim))
            return;

        float pct = Lifesteal_GetTotalPct(player);
        if (pct <= 0.0f)
            return;

        int32 heal = static_cast<int32>(damage * pct / 100.0f);
        if (heal <= 0)
            return;

        // Direct HP restore — NOT a heal spell, so it cannot re-enter the damage
        // path (hook-safety). ModifyHealth clamps at max HP, so no overheal.
        player->ModifyHealth(heal);

        // SANCTUM on-crit hook — remember this heal in case a real-crit signal
        // for this same hit arrives momentarily (see Lifesteal_NotifyRealCrit).
        g_pendingCritHeal[player->GetGUID().GetCounter()] = { heal, getMSTime() };
    }

    // SANCTUM on-crit hook — real SPELL crit signal (player casters only,
    // enforced at the Spell.cpp call site).
    void OnUnitSpellCrit(Unit* caster, Unit* /*victim*/, uint32 /*damage*/, SpellInfo const* /*spellInfo*/) override
    {
        if (Player* p = caster ? caster->ToPlayer() : nullptr)
            Lifesteal_NotifyRealCrit(p);
    }
};

class LifestealPlayerScript : public PlayerScript
{
public:
    LifestealPlayerScript() : PlayerScript("LifestealPlayerScript") {}

    void OnPlayerLogout(Player* player) override
    {
        if (player)
        {
            g_lifestealBonusPct.erase(player->GetGUID().GetCounter());
            g_pendingCritHeal.erase(player->GetGUID().GetCounter());
        }
    }

    // SANCTUM on-crit hook — real MELEE/RANGED crit signal.
    bool OnPlayerCanCastItemCombatSpell(Player* player, Unit* /*target*/, WeaponAttackType /*attType*/,
        uint32 /*procVictim*/, uint32 procEx, Item* /*item*/, ItemTemplate const* /*proto*/) override
    {
        if (player && (procEx & PROC_EX_CRITICAL_HIT))
            Lifesteal_NotifyRealCrit(player);
        return true;
    }
};

class LifestealWorldScript : public WorldScript
{
public:
    LifestealWorldScript() : WorldScript("LifestealWorldScript") {}

    void OnStartup() override
    {
        LOG_INFO("module", "[mod-lifesteal] Module loaded.");
    }
};

void AddSC_mod_lifesteal()
{
    new LifestealUnitScript();
    new LifestealPlayerScript();
    new LifestealWorldScript();
}
