// mod-dk-rework.cpp
// Sanctum Death Knight Rework
//
// Makes Death Knights start at level 1 alongside all other classes.
// Removes all ties to the Acherus starter zone.
// Auto-grants DK trainer spells as the player levels up (1-80).
// Makes DK runes always available — all rune cooldowns reset to 0 after every cast.
//
// Requires the companion SQL file (mod_dk_rework_world.sql) which:
//   1. Rescales DK trainer spell reqLevel from the 55-80 range to the 1-80 range.
//   2. Updates playercreateinfo for class 6 (DK) to spawn at Light's Hope Chapel
//      instead of Acherus (map 609).
//
// Flow:
//   1. Player creates a Death Knight character.
//   2. OnPlayerCreate fires — sets level to 1, grants spell 50977 (Death Gate)
//      to bypass Acherus locks, and grants any DK trainer spells available at level 1.
//   3. On every login, spell 50977 is re-confirmed and rune state is reset.
//   4. On every level-up, newly unlocked DK trainer spells are auto-granted.
//   5. On every spell cast by a DK, all 6 rune cooldowns are set to 0 so
//      runes are instantly re-available (no recharge delay).

#include "ScriptMgr.h"
#include "Player.h"
#include "SpellMgr.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Log.h"
#include "Spell.h"
#include "ObjectMgr.h"
#include "SpellScript.h"
#include "SpellAuraEffects.h"
#include <unordered_set>

// ============================================================
// Constants
// ============================================================

// Spell 50977: Death Gate — the portal spell DKs receive after leaving Acherus.
// AzerothCore uses HasSpell(50977) in several places to check whether a DK has
// "graduated" from the starter zone.  Granting it at creation bypasses all those
// guards without touching core files.
static const uint32 SPELL_DEATH_GATE         = 50977;

// Spell 49576: Death Grip — granted at creation so DKs have a signature ability
// right at level 1.
static const uint32 SPELL_DEATH_GRIP         = 49576;

// There are exactly 6 rune slots in WotLK (2 Blood, 2 Frost, 2 Unholy).
static const uint8  MAX_RUNE_SLOTS           = 6;

// ============================================================
// Helper: send a colored system message to a player
// ============================================================

static void Notify(Player* player, const std::string& msg)
{
    ChatHandler(player->GetSession()).SendSysMessage(msg.c_str());
}

// ============================================================
// Helper: detect whether a player has DK as a secondary/tertiary class.
// Queries character_multiclass once per login; result cached in s_isSecondaryDK.
// ============================================================

static bool QueryIsSecondaryDK(uint32 lowGuid)
{
    QueryResult res = CharacterDatabase.Query(
        "SELECT class1, class2, class3 FROM character_multiclass WHERE guid = {}",
        lowGuid);
    if (!res)
        return false;
    Field* f = res->Fetch();
    uint8 c1 = f[0].Get<uint8>();
    uint8 c2 = f[1].Get<uint8>();
    uint8 c3 = f[2].Get<uint8>();
    return (c1 != CLASS_DEATH_KNIGHT) &&
           (c2 == CLASS_DEATH_KNIGHT || c3 == CLASS_DEATH_KNIGHT);
}

// ============================================================
// Rank tracking maps for disease DoT scaling.
// Keyed by caster GUID. Updated when the applicator spell hits;
// read when the disease ticks.
// ============================================================

static std::unordered_map<uint64, uint8> s_lastIcyTouchRank;
static std::unordered_map<uint64, uint8> s_lastPlagueStrikeRank;

// Session cache: true if the player has DK as class2 or class3 (not primary class).
// Populated at OnPlayerLogin via character_multiclass query; evicted at OnPlayerLogout.
static std::unordered_map<uint32, bool> s_isSecondaryDK;

// ============================================================
// Helper: grant all DK trainer spells up to the player's current level.
//
// Works exactly like GrantClassSpells in mod-multiclass.
// Uses two DB queries:
//   1. playercreateinfo_spell_custom — passive / proficiency spells.
//   2. npc_trainer rows for trainer_class = 6 (Death Knight) where
//      reqLevel <= player's current level.
//
// The companion SQL rescaled reqLevel values so they span 1-80 instead
// of the default 55-80, giving proper progressive unlocks.
//
// HasSpell() guards every learnSpell() call so this is safe to call
// repeatedly on login and level-up.
// ============================================================

static void GrantDKSpells(Player* player)
{
    uint32 classMask = (1u << (CLASS_DEATH_KNIGHT - 1)); // bit 5

    // --- Passive / proficiency creation spells ---
    QueryResult startSpells = WorldDatabase.Query(
        "SELECT DISTINCT Spell FROM playercreateinfo_spell_custom "
        "WHERE (classmask & {}) AND classmask != 0",
        classMask
    );
    if (startSpells)
    {
        do
        {
            uint32 spellId = (*startSpells)[0].Get<uint32>();
            if (spellId && !player->HasSpell(spellId))
                player->learnSpell(spellId, false);
        } while (startSpells->NextRow());
    }

    // --- Trainer spells up to current level ---
    // trainer.Requirement = 6 (Death Knight), trainer.Type = 0 (class trainer).
    QueryResult trainerSpells = WorldDatabase.Query(
        "SELECT DISTINCT ts.SpellId "
        "FROM trainer_spell ts "
        "INNER JOIN trainer t ON t.Id = ts.TrainerId "
        "WHERE t.Requirement = {} AND t.Type = 0 "
        "AND ts.SpellId > 0 "
        "AND (ts.ReqLevel = 0 OR ts.ReqLevel <= {})",
        CLASS_DEATH_KNIGHT, player->GetLevel()
    );
    if (trainerSpells)
    {
        do
        {
            uint32 spellId = (*trainerSpells)[0].Get<uint32>();
            if (spellId && !player->HasSpell(spellId))
                player->learnSpell(spellId, false);
        } while (trainerSpells->NextRow());
    }

    // --- Rank 1 core abilities (Acherus quest rewards, not in trainer table) ---
    // In live WoW these are taught during the Acherus intro zone at level 55.
    // Sanctum bypasses Acherus, so the trainer only has ranks 2+. Without rank 1
    // the higher trainer ranks are unreachable. Staggered to match Sanctum's
    // 1-80 progression rather than dumping everything at creation.
    uint8 level = player->GetLevel();

    // Level 1 — disease appliers, core of every DK spec
    if (level >= 1)
    {
        if (!player->HasSpell(45477)) player->learnSpell(45477, false); // Icy Touch R1
        if (!player->HasSpell(45462)) player->learnSpell(45462, false); // Plague Strike R1
    }
    // Level 4 — Blood rune spender, pairs with diseases already active
    if (level >= 4)
    {
        if (!player->HasSpell(45902)) player->learnSpell(45902, false); // Blood Strike R1
    }
    // Level 7 — Runic Power dump, arrives with Blood Presence at 7
    if (level >= 7)
    {
        if (!player->HasSpell(47541)) player->learnSpell(47541, false); // Death Coil R1
    }
    // Level 10 — Frost spec's RP dump
    if (level >= 10)
    {
        if (!player->HasSpell(49143)) player->learnSpell(49143, false); // Frost Strike R1
    }
    // Level 14 — Blood DK's primary dual-strike
    if (level >= 14)
    {
        if (!player->HasSpell(55050)) player->learnSpell(55050, false); // Heart Strike R1
    }
    // Level 17 — Unholy spec's physical+shadow primary, same level as Death & Decay
    if (level >= 17)
    {
        if (!player->HasSpell(55090)) player->learnSpell(55090, false); // Scourge Strike R1
    }
    // Level 20 — Unholy AoE utility, same level as Path of Frost + Obliterate R1
    if (level >= 20)
    {
        if (!player->HasSpell(49158)) player->learnSpell(49158, false); // Corpse Explosion R1
    }
}

// ============================================================
// Helper: reset all rune cooldowns to zero
// Called after every spell cast so runes are always immediately available.
// SetRuneCooldown(index, 0) also calls SetRuneState(index, true) internally,
// which marks the rune as ready in the engine's power tracking.
// ResyncRunes sends the updated rune state packet to the client.
// ============================================================

static void ResetRuneCooldowns(Player* player)
{
    for (uint8 i = 0; i < MAX_RUNE_SLOTS; ++i)
        player->SetRuneCooldown(i, 0);
    player->ResyncRunes(MAX_RUNE_SLOTS);
}

// ============================================================
// SpellScript: Icy Touch (R1-R5)
//
// Replaces vanilla damage with Sanctum values tuned for the 1-80 curve.
// Also records what rank was cast so Frost Fever's DoT tick can scale
// to match (stored in s_lastIcyTouchRank keyed by caster GUID).
//
// Spell IDs: 45477 R1 / 49896 R2 / 49903 R3 / 49904 R4 / 49909 R5
// School: Frost   Effect: SPELL_EFFECT_SCHOOL_DAMAGE (EFFECT_0)
// SP direct coeff: 0.429
// ============================================================

class spell_dk_icy_touch : public SpellScript
{
    PrepareSpellScript(spell_dk_icy_touch);

    void HandleDamage()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        int32 minDmg = 0, maxDmg = 0;
        uint8 rank = 1;
        switch (m_scriptSpellId)
        {
            case 45477: minDmg = 14;  maxDmg = 18;  rank = 1; break;
            case 49896: minDmg = 38;  maxDmg = 46;  rank = 2; break;
            case 49903: minDmg = 88;  maxDmg = 104; rank = 3; break;
            case 49904: minDmg = 188; maxDmg = 220; rank = 4; break;
            case 49909: minDmg = 370; maxDmg = 430; rank = 5; break;
            default: return;
        }

        s_lastIcyTouchRank[caster->GetGUID().GetRawValue()] = rank;

        int32 spBonus = int32(caster->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FROST) * 0.429f);
        int32 base = irand(minDmg, maxDmg);
        SetHitDamage(base + spBonus);
    }

    void Register() override
    {
        // OnHit has no effect-type validation — fires for all ranks regardless of DBC layout.
        OnHit += SpellHitFn(spell_dk_icy_touch::HandleDamage);
    }
};

// ============================================================
// SpellScript: Plague Strike (R1-R6)
//
// Physical + Shadow damage strike that also applies Blood Plague.
// Records rank in s_lastPlagueStrikeRank for Blood Plague tick scaling.
//
// Spell IDs: 45462 R1 / 49917 R2 / 49918 R3 / 49919 R4 / 49920 R5 / 49921 R6
// School: Physical+Shadow   Effect: SPELL_EFFECT_SCHOOL_DAMAGE (EFFECT_1)
//   Note: Plague Strike EFFECT_0 is SPELL_EFFECT_WEAPON_PERCENT_DAMAGE (the
//   weapon swing). EFFECT_1 is the shadow school damage component. We hook
//   EFFECT_1 to override the shadow component; the engine handles the weapon
//   swing on EFFECT_0 without interference.
// AP direct coeff: 0.20
// ============================================================

class spell_dk_plague_strike : public SpellScript
{
    PrepareSpellScript(spell_dk_plague_strike);

    void HandleDamage()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        int32 minDmg = 0, maxDmg = 0;
        uint8 rank = 1;
        switch (m_scriptSpellId)
        {
            case 45462: minDmg = 18;  maxDmg = 24;  rank = 1; break;
            case 49917: minDmg = 46;  maxDmg = 58;  rank = 2; break;
            case 49918: minDmg = 104; maxDmg = 128; rank = 3; break;
            case 49919: minDmg = 220; maxDmg = 270; rank = 4; break;
            case 49920: minDmg = 430; maxDmg = 520; rank = 5; break;
            case 49921: minDmg = 760; maxDmg = 910; rank = 6; break;
            default: return;
        }

        // Record rank so Blood Plague's AuraScript knows the correct tick value.
        s_lastPlagueStrikeRank[caster->GetGUID().GetRawValue()] = rank;

        int32 apBonus = int32(caster->GetTotalAttackPowerValue(BASE_ATTACK) * 0.20f);
        int32 base = irand(minDmg, maxDmg);
        SetHitDamage(base + apBonus);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_dk_plague_strike::HandleDamage);
    }
};

// ============================================================
// SpellScript: Frost Strike (R1-R6)
//
// Runic Power finisher. EFFECT_0 is SPELL_EFFECT_WEAPON_PERCENT_DAMAGE —
// we let the engine calculate the weapon % normally, then ADD our flat
// bonus and AP scaling on top. This preserves weapon speed/crit/parry
// logic that the engine provides for weapon damage effects.
//
// Spell IDs: 49143 R1 / 51416 R2 / 51417 R3 / 51418 R4 / 51419 R5 / 55268 R6
// AP coeff: 0.38
// ============================================================

class spell_dk_frost_strike : public SpellScript
{
    PrepareSpellScript(spell_dk_frost_strike);

    void HandleDamage()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        int32 flat = 0;
        switch (m_scriptSpellId)
        {
            case 49143: flat = 25;   break;
            case 51416: flat = 65;   break;
            case 51417: flat = 146;  break;
            case 51418: flat = 310;  break;
            case 51419: flat = 625;  break;
            case 55268: flat = 1120; break;
            default: return;
        }

        int32 apBonus = int32(caster->GetTotalAttackPowerValue(BASE_ATTACK) * 0.38f);
        // GetHitDamage() returns what the engine already computed from weapon%.
        // We add our bonus on top rather than replacing, so the engine's
        // weapon percent, crit multiplier, and armor reduction all still apply.
        SetHitDamage(GetHitDamage() + flat + apBonus);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_dk_frost_strike::HandleDamage);
    }
};

// ============================================================
// SpellScript: Heart Strike (R1-R6)
//
// Blood cleave that hits two targets. Primary = explicit target.
// Secondary target (if any) gets 50% of primary damage.
// Damage increases by 3% per active disease on the target.
//
// Diseases to check: Frost Fever (55095), Blood Plague (55078).
//
// Spell IDs: 55050 R1 / 55258 R2 / 55259 R3 / 55260 R4 / 55261 R5 / 55262 R6
// Effect: SPELL_EFFECT_WEAPON_PERCENT_DAMAGE — we ADD flat + AP scaling.
// AP coeff: 0.22
// ============================================================

class spell_dk_heart_strike : public SpellScript
{
    PrepareSpellScript(spell_dk_heart_strike);

    // Store primary target damage after we compute it on the first hit so the
    // secondary target handler can apply 50%.
    int32 m_primaryDamage = 0;

    void HandleDamage()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        int32 minDmg = 0, maxDmg = 0;
        switch (m_scriptSpellId)
        {
            case 55050: minDmg = 18;   maxDmg = 28;   break;
            case 55258: minDmg = 48;   maxDmg = 72;   break;
            case 55259: minDmg = 110;  maxDmg = 164;  break;
            case 55260: minDmg = 238;  maxDmg = 356;  break;
            case 55261: minDmg = 480;  maxDmg = 720;  break;
            case 55262: minDmg = 880;  maxDmg = 1320; break;
            default: return;
        }

        int32 apBonus = int32(caster->GetTotalAttackPowerValue(BASE_ATTACK) * 0.22f);
        int32 base    = irand(minDmg, maxDmg);

        // Count active diseases on the explicit primary target (not each individual hit target).
        // Both hits benefit from diseases on the primary target.
        Unit* explTarget = GetExplTargetUnit();
        int32 diseaseCount = 0;
        if (explTarget)
        {
            if (explTarget->HasAura(55095, caster->GetGUID())) ++diseaseCount; // Frost Fever
            if (explTarget->HasAura(55078, caster->GetGUID())) ++diseaseCount; // Blood Plague
        }
        float diseaseMultiplier = 1.0f + (0.03f * diseaseCount);

        if (target == explTarget)
        {
            // Primary target — compute and store full damage.
            m_primaryDamage = int32((base + apBonus) * diseaseMultiplier);
            SetHitDamage(GetHitDamage() + m_primaryDamage);
        }
        else
        {
            // Secondary (cleave) target — 50% of primary.
            SetHitDamage(GetHitDamage() + int32(m_primaryDamage * 0.5f));
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_dk_heart_strike::HandleDamage);
    }
};

// ============================================================
// SpellScript: Scourge Strike (R1-R4)
//
// Physical hit + shadow bonus per disease on the target.
// EFFECT_0 is SPELL_EFFECT_SCHOOL_DAMAGE (shadow). Physical swing is
// EFFECT_1 (SPELL_EFFECT_WEAPON_PERCENT_DAMAGE) — left to engine.
// We set EFFECT_0 to the shadow component only; the engine handles physical.
//
// Shadow damage per disease is added on top via SetHitDamage.
// Diseases: Frost Fever (55095), Blood Plague (55078).
//
// Spell IDs: 55090 R1 / 55265 R2 / 55270 R3 / 55271 R4
// AP coeff: 0.22
// ============================================================

class spell_dk_scourge_strike_custom : public SpellScript
{
    PrepareSpellScript(spell_dk_scourge_strike_custom);

    void HandleShadowDamage()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target)
            return;

        int32 physMin = 0, physMax = 0;
        int32 shadowPerDiseaseMin = 0, shadowPerDiseaseMax = 0;
        switch (m_scriptSpellId)
        {
            case 55090: physMin = 22;  physMax = 35;  shadowPerDiseaseMin = 6;  shadowPerDiseaseMax = 10;  break;
            case 55265: physMin = 58;  physMax = 92;  shadowPerDiseaseMin = 16; shadowPerDiseaseMax = 26;  break;
            case 55270: physMin = 130; physMax = 206; shadowPerDiseaseMin = 36; shadowPerDiseaseMax = 58;  break;
            case 55271: physMin = 278; physMax = 440; shadowPerDiseaseMin = 76; shadowPerDiseaseMax = 122; break;
            default: return;
        }

        // Count active diseases this caster has on the target.
        int32 diseaseCount = 0;
        if (target->HasAura(55095, caster->GetGUID())) ++diseaseCount; // Frost Fever
        if (target->HasAura(55078, caster->GetGUID())) ++diseaseCount; // Blood Plague

        int32 apBonus       = int32(caster->GetTotalAttackPowerValue(BASE_ATTACK) * 0.22f);
        int32 physBase      = irand(physMin, physMax) + apBonus;
        int32 shadowBase    = irand(shadowPerDiseaseMin, shadowPerDiseaseMax) * diseaseCount;

        // EFFECT_0 is the shadow school damage — set it to physical + shadow total.
        // The engine's EFFECT_1 weapon percent damage still fires separately.
        SetHitDamage(physBase + shadowBase);
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_dk_scourge_strike_custom::HandleShadowDamage);
    }
};

// ============================================================
// AuraScript: Frost Fever (spell 55095)
//
// Disease DoT applied by Icy Touch. Tick value scales with what rank
// of Icy Touch applied it, read from s_lastIcyTouchRank.
//
// Tick values: R1=4, R2=10, R3=22, R4=46, R5=88
// SP tick coeff: 0.13
// ============================================================

class spell_dk_frost_fever : public AuraScript
{
    PrepareAuraScript(spell_dk_frost_fever);

    void HandleCalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* caster = GetCaster();
        if (!caster)
            return;

        // tickDmg[rank] — index 0 is unused, valid ranks 1-5.
        static const int32 tickDmg[6] = { 0, 4, 10, 22, 46, 88 };

        uint8 rank = 1;
        auto it = s_lastIcyTouchRank.find(caster->GetGUID().GetRawValue());
        if (it != s_lastIcyTouchRank.end())
            rank = it->second;
        if (rank < 1 || rank > 5)
            rank = 1;

        int32 spBonus = int32(caster->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FROST) * 0.13f);
        amount = tickDmg[rank] + spBonus;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dk_frost_fever::HandleCalcAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// ============================================================
// AuraScript: Blood Plague (spell 55078)
//
// Disease DoT applied by Plague Strike. Tick value scales with what rank
// of Plague Strike applied it, read from s_lastPlagueStrikeRank.
//
// Tick values: R1=5, R2=12, R3=28, R4=58, R5=112, R6=195
// AP tick coeff: 0.055
// ============================================================

class spell_dk_blood_plague : public AuraScript
{
    PrepareAuraScript(spell_dk_blood_plague);

    void HandleCalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* caster = GetCaster();
        if (!caster)
            return;

        // tickDmg[rank] — index 0 is unused, valid ranks 1-6.
        static const int32 tickDmg[7] = { 0, 5, 12, 28, 58, 112, 195 };

        uint8 rank = 1;
        auto it = s_lastPlagueStrikeRank.find(caster->GetGUID().GetRawValue());
        if (it != s_lastPlagueStrikeRank.end())
            rank = it->second;
        if (rank < 1 || rank > 6)
            rank = 1;

        int32 apBonus = int32(caster->GetTotalAttackPowerValue(BASE_ATTACK) * 0.055f);
        amount = tickDmg[rank] + apBonus;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dk_blood_plague::HandleCalcAmount, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// ============================================================
// Player Script
// ============================================================

class DKReworkPlayerScript : public PlayerScript
{
public:
    DKReworkPlayerScript() : PlayerScript("DKReworkPlayerScript") {}

    // --------------------------------------------------------
    // OnPlayerCreate — fires when a new character is first created,
    // before the very first login.
    // --------------------------------------------------------
    void OnPlayerCreate(Player* player) override
    {
        if (player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        // Grant Death Gate (spell 50977).
        // AzerothCore checks HasSpell(50977) in three places:
        //   • Player::GetStartPosition()    — returns Eastern Kingdoms coords instead of Acherus
        //   • Player.cpp teleport guard     — allows leaving MAP_EBON_HOLD (609)
        //   • Group/BG handlers             — same teleport guard
        // With this spell the character is treated as a "graduated" DK everywhere
        // in the core without any core modifications.
        if (!player->HasSpell(SPELL_DEATH_GATE))
            player->learnSpell(SPELL_DEATH_GATE, false);

        // Grant Death Grip at creation so DKs have their signature ability from level 1.
        if (!player->HasSpell(SPELL_DEATH_GRIP))
            player->learnSpell(SPELL_DEATH_GRIP, false);

        LOG_INFO("module", "[mod-dk-rework] DK '{}' (GUID {}) created — Acherus bypassed.",
            player->GetName(), player->GetGUID().GetCounter());
    }

    // --------------------------------------------------------
    // OnPlayerLogin — fires every time the player logs in.
    // --------------------------------------------------------
    void OnPlayerLogin(Player* player) override
    {
        uint32 lowGuid = static_cast<uint32>(player->GetGUID().GetCounter());

        // Detect and cache secondary-DK status for this session.
        bool isSecondary = QueryIsSecondaryDK(lowGuid);
        s_isSecondaryDK[lowGuid] = isSecondary;

        // Secondary DK: the engine never opens a Runic Power pool for non-primary DKs.
        // SetMaxPower must be called first — without it, SetPower clamps to 0 and every
        // RP-spending DK spell fails. Pool max 1000 matches the primary DK pool size.
        if (isSecondary)
        {
            player->SetMaxPower(POWER_RUNIC_POWER, 1000);
            player->SetPower(POWER_RUNIC_POWER, 1000);
        }

        if (player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        // --- everything below is the existing primary-DK login logic, unchanged ---

        // Safety net for DKs that existed before this module was installed.
        if (!player->HasSpell(SPELL_DEATH_GATE))
        {
            player->learnSpell(SPELL_DEATH_GATE, false);
            Notify(player, "|cffC41F3B[Sanctum]|r Your Death Knight class has been updated. Welcome to Sanctum.");
        }

        // On first login, strip Acherus starter gear forced by CharStartOutfit.dbc.
        // DestroyItem here persists correctly — OnPlayerCreate fires after SaveToDB
        // so any removals there are lost; OnPlayerLogin is the earliest safe window.
        if (player->HasAtLoginFlag(AT_LOGIN_FIRST))
        {
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            }
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            {
                if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                    player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            }
        }

        // If this DK is still at the default heroic starting level (55), force to 1.
        // SetLevel() is safe to call here — the player is fully in the world by the
        // time OnPlayerLogin fires, unlike OnPlayerCreate where it broke saves.
        if (player->GetLevel() == 55)
        {
            player->SetLevel(1);

            // Unit::SetLevel only writes UNIT_FIELD_LEVEL — base stats are NOT
            // recomputed. Re-init them from level-1 DB tables to match a true
            // level-1 character, mirroring the Player::GiveLevel() pattern.
            PlayerLevelInfo info;
            sObjectMgr->GetPlayerLevelInfo(player->getRace(true), player->getClass(), 1, &info);
            PlayerClassLevelInfo classInfo;
            sObjectMgr->GetPlayerClassLevelInfo(player->getClass(), 1, &classInfo);

            for (uint8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
                player->SetCreateStat(Stats(i), info.stats[i]);
            player->SetCreateHealth(classInfo.basehealth);
            player->SetCreateMana(classInfo.basemana);

            player->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr->GetXPForLevel(1));

            player->InitStatsForLevel(true);
            player->InitTalentForLevel();
            player->InitTaxiNodesForLevel();
            player->InitGlyphsForLevel();
            player->UpdateAllStats();
            player->UpdateSkillsForLevel();

            player->SetFullHealth();
            player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
        }

        // On first login only, strip every vanilla DK combat spell that the engine
        // auto-granted via learnSkillRewardedSpells (which uses the level-55 skill values
        // from character creation). GrantDKSpells() re-grants level-appropriate ranks
        // immediately below. The strip is safe to run whether or not the level-55 reset
        // fired above — first-login DKs always need it.
        if (player->HasAtLoginFlag(AT_LOGIN_FIRST))
        {
            // Spells to keep even though they appear in the trainer table.
            static const std::unordered_set<uint32> kStripExempt = {
                SPELL_DEATH_GATE,   // 50977 — Acherus bypass, re-checked above
                SPELL_DEATH_GRIP,   // 49576 — granted at creation, stays at level 1
                53428               // Runeforging — not in trainer table anyway, defensive
            };

            QueryResult stripQuery = WorldDatabase.Query(
                "SELECT DISTINCT ts.SpellId "
                "FROM trainer_spell ts "
                "INNER JOIN trainer t ON t.Id = ts.TrainerId "
                "WHERE t.Requirement = {} AND t.Type = 0 AND ts.SpellId > 0",
                CLASS_DEATH_KNIGHT
            );
            if (stripQuery)
            {
                do
                {
                    uint32 spellId = (*stripQuery)[0].Get<uint32>();
                    if (spellId
                        && kStripExempt.find(spellId) == kStripExempt.end()
                        && player->HasSpell(spellId))
                    {
                        player->removeSpell(spellId, SPEC_MASK_ALL, false);
                    }
                } while (stripQuery->NextRow());
            }
        }

        // Re-grant all DK spells up to current level.
        GrantDKSpells(player);

        // All runes ready on login.
        ResetRuneCooldowns(player);
    }

    // --------------------------------------------------------
    // OnPlayerLevelChanged — fires every time the player gains a level.
    // --------------------------------------------------------
    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        // Grant any DK trainer spells newly unlocked at the new level.
        GrantDKSpells(player);
    }

    // --------------------------------------------------------
    // OnPlayerSpellCast — fires in Spell::Cast() for every spell the player casts.
    //
    // Rune cooldown design:
    //   In standard WotLK, each rune has a 10-second recharge after use.
    //   Sanctum design: "DK runes: always available, no cooldown."
    //   We implement this by immediately zeroing all rune cooldowns after each cast.
    //   The client receives the updated rune state via ResyncRunes and shows
    //   all runes as instantly ready.
    // --------------------------------------------------------
    void OnPlayerSpellCast(Player* player, Spell* /*spell*/, bool /*skipCheck*/) override
    {
        uint32 lowGuid = static_cast<uint32>(player->GetGUID().GetCounter());

        // Secondary DK: refill RP after every cast. This hook fires after TakePower(),
        // so the cast just made was funded by the previous top-up; this refill funds
        // the next cast. Do NOT call ResetRuneCooldowns — secondary DKs have no
        // DK-primary rune pool and calling ResyncRunes on a non-DK-primary sends
        // garbage rune packets to the client.
        auto it = s_isSecondaryDK.find(lowGuid);
        if (it != s_isSecondaryDK.end() && it->second)
        {
            player->SetPower(POWER_RUNIC_POWER, 1000);
            return;
        }

        if (player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        ResetRuneCooldowns(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        s_isSecondaryDK.erase(static_cast<uint32>(player->GetGUID().GetCounter()));
    }
};

// ============================================================
// Registration
// ============================================================

void AddSC_mod_dk_rework()
{
    new DKReworkPlayerScript();
    RegisterSpellScript(spell_dk_icy_touch);
    RegisterSpellScript(spell_dk_plague_strike);
    RegisterSpellScript(spell_dk_frost_strike);
    RegisterSpellScript(spell_dk_heart_strike);
    RegisterSpellScript(spell_dk_scourge_strike_custom);
    RegisterSpellScript(spell_dk_frost_fever);
    RegisterSpellScript(spell_dk_blood_plague);
    LOG_INFO("module", "[mod-dk-rework] Module loaded. Death Knights start at level 1.");
}
