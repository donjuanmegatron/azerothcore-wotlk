// aa_archetype.cpp
//
// Sanctum AA System — Archetype tree hook-based AAs.
//
// Stat passives (Stalwart, Critical Healing, Spirit Channel) are in mod-aa-system.cpp
// ApplyAAStat. This file handles effects that require combat hooks.
//
// IMPLEMENTED HERE:
//   4102  Iron Resolve    — -3/6/10% DR while being attacked (holds top threat)
//   4104  Last Stand      — ONE-SHOT: drop below 25% HP → 20% DR for 8s (90s ICD)
//   4204  Apex Predator   — ONE-SHOT: on kill, +30% of highest primary stat for 15s
//
// DEFERRED (missing hooks or requires custom spells):
//   4101  Stalwart        — block value (+50/100/150 flat) — in ApplyAAStat
//   4103  Anchored        — LIVE: OnAuraApply reduces stun/knockback duration 25/50/75%
//   4106  Double Riposte  — no after-dodge/parry hook
//   4201  Bloodletting    — crit flag not exposed in damage hooks
//   4202  Haste Surge     — mod-haste not built yet
//   4203  Mortal Strike   — requires reliable custom debuff spell ID
//   4205  Weapon Fury     — active ability, deferred to aa_actives.cpp
//   4301  Mending Touch   — SPELL_AURA_MOD_HEALING_DONE_PCT needs custom aura
//   4302  Critical Healing — in ApplyAAStat
//   4303  Spirit Channel  — in ApplyAAStat
//   4304  Lingering Renewal — no OnHeal hook to inject a HoT
//   4305  Battle Mender   — mana cost reduction needs per-spell SpellMod
//   4306  Overflowing     — no excess-heal hook available
//   4307  Chain Healing   — no OnHeal hook available

#include "aa_runtime.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Unit.h"
#include "SpellInfo.h"
#include "Timer.h"
#include <unordered_map>
#include <algorithm>

// ---------------------------------------------------------------------------
// File-local runtime state
// ---------------------------------------------------------------------------
namespace
{
    struct LastStandState { uint32 untilMs = 0; uint32 icdMs = 0; };
    struct ApexState      { float  bonus   = 0.0f; uint32 untilMs = 0; bool applied = false; };

    std::unordered_map<uint32, LastStandState> g_lastStand;
    std::unordered_map<uint32, ApexState>      g_apex;

    static inline Player* AsPlayer(Unit* u)
    {
        return (u && u->IsPlayer()) ? u->ToPlayer() : nullptr;
    }

    // Clamp rank to array index (arrays sized [0..3]).
    template<typename T>
    static inline T Idx(uint8 rank) { return static_cast<T>(std::min<uint8>(rank, 3)); }

    static void ClearPlayerState(uint32 guid)
    {
        // Remove Apex Predator stat burst if it was still active
        auto apexIt = g_apex.find(guid);
        // (stat removal handled in OnUnitUpdate expiry path or on logout below)
        g_lastStand.erase(guid);
        g_apex.erase(guid);
    }
}

// ---------------------------------------------------------------------------
// aa_archetype_unit — UnitScript for hook-based archetype AAs
// ---------------------------------------------------------------------------
class aa_archetype_unit : public UnitScript
{
public:
    aa_archetype_unit() : UnitScript("aa_archetype_unit", true,
    {
        UNITHOOK_ON_DAMAGE,
        UNITHOOK_MODIFY_MELEE_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_ON_UNIT_UPDATE,
        UNITHOOK_ON_UNIT_DEATH,
        UNITHOOK_ON_AURA_APPLY,
    }) {}

    // -----------------------------------------------------------------------
    // OnDamage — fires after damage is applied; used to TRIGGER state.
    //   Last Stand — when player drops below 25% HP, arm 20% DR for 8s
    // -----------------------------------------------------------------------
    void OnDamage(Unit* attacker, Unit* victim, uint32& /*damage*/) override
    {
        Player* player = AsPlayer(victim);
        if (!player)
            return;

        uint32 guid = player->GetGUID().GetCounter();

        // Last Stand (one-shot) — arm when falling below 25% HP
        if (SanctumAA::Has(player, AA_T_LAST_STAND))
        {
            auto& ls = g_lastStand[guid];
            if (player->GetHealthPct() < 25.0f && GetMSTimeDiffToNow(ls.icdMs) >= 90000u)
            {
                uint32 now = getMSTime();
                ls.untilMs = now + 8000u;
                ls.icdMs   = now;
            }
        }
    }

    // -----------------------------------------------------------------------
    // ModifyMeleeDamage — victim-side: Iron Resolve + Last Stand DR.
    // -----------------------------------------------------------------------
    void ModifyMeleeDamage(Unit* target, Unit* /*attacker*/, uint32& damage) override
    {
        if (damage == 0)
            return;

        Player* player = AsPlayer(target);
        if (!player)
            return;

        uint32 guid = player->GetGUID().GetCounter();

        // Iron Resolve — -3/6/10% DR while being actively attacked
        {
            uint8 rank = SanctumAA::GetRank(player, AA_T_IRON_RESOLVE);
            if (rank > 0 && !player->getAttackers().empty())
            {
                static const float dr[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                damage = (uint32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
            }
        }

        // Last Stand — 20% DR for 8s after dropping below 25% HP
        {
            auto it = g_lastStand.find(guid);
            if (it != g_lastStand.end() && getMSTime() < it->second.untilMs)
                damage = (uint32)(damage * 0.80f);
        }
    }

    // -----------------------------------------------------------------------
    // ModifySpellDamageTaken — victim-side: Iron Resolve + Last Stand DR.
    // -----------------------------------------------------------------------
    void ModifySpellDamageTaken(Unit* target, Unit* /*attacker*/, int32& damage, SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0)
            return;

        Player* player = AsPlayer(target);
        if (!player)
            return;

        uint32 guid = player->GetGUID().GetCounter();

        // Iron Resolve
        {
            uint8 rank = SanctumAA::GetRank(player, AA_T_IRON_RESOLVE);
            if (rank > 0 && !player->getAttackers().empty())
            {
                static const float dr[] = { 0.0f, 0.03f, 0.06f, 0.10f };
                damage = (int32)(damage * (1.0f - dr[Idx<uint8>(rank)]));
            }
        }

        // Last Stand
        {
            auto it = g_lastStand.find(guid);
            if (it != g_lastStand.end() && getMSTime() < it->second.untilMs)
                damage = (int32)(damage * 0.80f);
        }
    }

    // -----------------------------------------------------------------------
    // OnAuraApply — Anchored: reduce stun/knockback duration on player.
    // -----------------------------------------------------------------------
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit->ToPlayer();
        if (!player)
            return;

        uint8 rank = SanctumAA::GetRank(player, AA_T_ANCHORED);
        if (!rank)
            return;

        SpellInfo const* info = aura->GetSpellInfo();
        if (!info)
            return;

        // Check if any effect has stun or knockback mechanic.
        bool isStun = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            uint32 mech = info->Effects[i].Mechanic;
            if (mech == MECHANIC_STUN || mech == MECHANIC_KNOCKOUT || mech == MECHANIC_BANISH)
            {
                isStun = true;
                break;
            }
        }
        // Also check the spell-level mechanic flag.
        if (!isStun && (info->Mechanic == MECHANIC_STUN || info->Mechanic == MECHANIC_KNOCKOUT))
            isStun = true;

        if (!isStun)
            return;

        static const float reduction[] = { 1.0f, 0.75f, 0.50f, 0.25f, 0.0f }; // rank 1/2/3/4 = 25/50/75/100%
        int32 newDur = (int32)(aura->GetDuration() * reduction[std::min<uint8>(rank, 4)]);
        aura->SetDuration(std::max(1, newDur)); // 1ms = effectively immune at rank 4
    }

    // -----------------------------------------------------------------------
    // OnUnitUpdate — expire Apex Predator stat burst when window closes.
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

        auto it = g_apex.find(guid);
        if (it != g_apex.end() && it->second.applied && now >= it->second.untilMs)
        {
            float bonus = it->second.bonus;
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    TOTAL_VALUE, bonus, false);
            player->UpdateAllStats();
            it->second.applied = false;
        }
    }

    // -----------------------------------------------------------------------
    // OnUnitDeath — clear player state on death.
    // -----------------------------------------------------------------------
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!unit->IsPlayer())
            return;

        uint32 guid = unit->GetGUID().GetCounter();

        // Remove Apex Predator stat burst immediately on death
        auto it = g_apex.find(guid);
        if (it != g_apex.end() && it->second.applied)
        {
            Player* player = unit->ToPlayer();
            float bonus = it->second.bonus;
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    TOTAL_VALUE, bonus, false);
            player->UpdateAllStats();
            it->second.applied = false;
        }

        ClearPlayerState(guid);
    }
};

// ---------------------------------------------------------------------------
// aa_archetype_player — PlayerScript for on-kill AAs
// ---------------------------------------------------------------------------
class aa_archetype_player : public PlayerScript
{
public:
    aa_archetype_player() : PlayerScript("aa_archetype_player") {}

    // Apex Predator — on kill, add 30% of highest primary stat to all primary stats for 15s.
    // Refreshes (removes old burst and applies new) on each kill while active.
    void OnPlayerCreatureKill(Player* player, Creature* /*creature*/) override
    {
        if (!SanctumAA::Has(player, AA_D_APEX_PREDATOR))
            return;

        float maxStat = 0.0f;
        maxStat = std::max(maxStat, (float)player->GetStat(STAT_STRENGTH));
        maxStat = std::max(maxStat, (float)player->GetStat(STAT_AGILITY));
        maxStat = std::max(maxStat, (float)player->GetStat(STAT_STAMINA));
        maxStat = std::max(maxStat, (float)player->GetStat(STAT_INTELLECT));
        maxStat = std::max(maxStat, (float)player->GetStat(STAT_SPIRIT));

        float  bonus = maxStat * 0.30f;
        uint32 guid  = player->GetGUID().GetCounter();
        auto&  a     = g_apex[guid];

        // Remove previous burst if still running before applying fresh one
        if (a.applied)
        {
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  TOTAL_VALUE, a.bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   TOTAL_VALUE, a.bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   TOTAL_VALUE, a.bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, a.bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    TOTAL_VALUE, a.bonus, false);
        }

        a.bonus   = bonus;
        a.untilMs = getMSTime() + 15000u;
        a.applied = true;

        player->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  TOTAL_VALUE, bonus, true);
        player->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   TOTAL_VALUE, bonus, true);
        player->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   TOTAL_VALUE, bonus, true);
        player->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, bonus, true);
        player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    TOTAL_VALUE, bonus, true);
        player->UpdateAllStats();
    }

    // Remove Apex burst and clear all state on logout
    void OnPlayerLogout(Player* player) override
    {
        uint32 guid = player->GetGUID().GetCounter();

        auto it = g_apex.find(guid);
        if (it != g_apex.end() && it->second.applied)
        {
            float bonus = it->second.bonus;
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, TOTAL_VALUE, bonus, false);
            player->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    TOTAL_VALUE, bonus, false);
            player->UpdateAllStats();
            it->second.applied = false;
        }

        ClearPlayerState(guid);
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void AddSC_aa_archetype()
{
    new aa_archetype_unit();
    new aa_archetype_player();
}
