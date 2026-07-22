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
// NOT in Phase 1 (flagged): "crits double lifesteal" — OnDamage carries no crit
// flag in this AC version (same limitation the AA/identity systems hit), so the
// crit-doubling is deferred. Phase 1 is the flat baseline + the integration API.
// -------------------------------------------------------------------------

#include "ScriptMgr.h"
#include "Player.h"
#include "Unit.h"
#include "Log.h"
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
    }
};

class LifestealPlayerScript : public PlayerScript
{
public:
    LifestealPlayerScript() : PlayerScript("LifestealPlayerScript") {}

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            g_lifestealBonusPct.erase(player->GetGUID().GetCounter());
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
