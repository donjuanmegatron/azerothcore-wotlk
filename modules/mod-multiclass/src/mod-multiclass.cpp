// mod-multiclass.cpp
// Sanctum Multiclass System
//
// Allows each player to bind two additional classes beyond their creation class.
// The creation class stays primary (controls talent trees). Secondary and tertiary
// classes grant all their spells and abilities through an in-game NPC called the Class Weaver.
//
// Flow:
//   1. Player creates character (picks class 1 via normal WoW character creation screen).
//   2. OnCreate fires — their row is seeded in character_multiclass.
//   3. Player finds the Class Weaver NPC in Dalaran, Stormwind, or Orgrimmar.
//   4. NPC presents gossip menus to choose class 2, then class 3 (permanent, confirmed).
//   5. On each selection, spells for the new class are granted immediately.
//   6. OnLogin and OnLevelChanged always re-grant spells so nothing is missed.
//
// Resource handling for secondary/tertiary classes:
//   - WoW only tracks one power type per player (determined by primary class).
//   - Secondary Warrior / Rogue have no Rage or Energy bar of their own, yet their
//     abilities require those resources.
//   - Solution: every 500ms the WorldScript tops up Rage (secondary Warriors) and
//     Energy (secondary Rogues) directly in the unit data.  The server-side power
//     check reads those values, so abilities succeed even though the client never
//     shows a second resource bar.
//   - Primary Warriors and Rogues are left alone — they generate and spend their
//     resource normally.
//
// Stance / form requirements:
//   - Warrior abilities require Battle/Defensive/Berserker Stance.
//   - Rogue abilities are usable anywhere (no stance restriction normally).
//   - Druid abilities require specific shapeshift forms.
//   - At server startup StripShapeshiftRequirements() zeroes the Stances and
//     StancesNot mask on every Warrior, Rogue, and Druid spell so all abilities
//     are usable in any stance or form.  Primary Warriors / Druids also benefit
//     (stance-locked abilities become stance-free), which is intentional for Sanctum.

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "GossipDef.h"
#include "ScriptedGossip.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Log.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "DBCStores.h"

#include "SpellScript.h"
#include "CommandScript.h"
#include <unordered_map>
#include <sstream>

using namespace Acore::ChatCommands;

// ============================================================
// Constants
// ============================================================

static const uint32 MULTICLASS_NPC_ENTRY = 700100;


// Gossip action number layout:
//   1        = open class list for 2nd class
//   2        = open class list for 3rd class
//   100-111  = player clicked a class name (class ID is action - 100)
//   200-211  = player confirmed a class choice (class ID is action - 200)

// ============================================================
// WoW Class IDs (3.3.5a)
// Note: 10 does not exist in WotLK.
// ============================================================

enum WoWClass : uint8
{
    WOW_CLASS_WARRIOR      = 1,
    WOW_CLASS_PALADIN      = 2,
    WOW_CLASS_HUNTER       = 3,
    WOW_CLASS_ROGUE        = 4,
    WOW_CLASS_PRIEST       = 5,
    WOW_CLASS_DEATH_KNIGHT = 6,
    WOW_CLASS_SHAMAN       = 7,
    WOW_CLASS_MAGE         = 8,
    WOW_CLASS_WARLOCK      = 9,
    WOW_CLASS_DRUID        = 11
};

static const uint8 ALL_CLASSES[] = {
    WOW_CLASS_WARRIOR, WOW_CLASS_PALADIN, WOW_CLASS_HUNTER,  WOW_CLASS_ROGUE,
    WOW_CLASS_PRIEST,  WOW_CLASS_DEATH_KNIGHT, WOW_CLASS_SHAMAN,
    WOW_CLASS_MAGE,    WOW_CLASS_WARLOCK, WOW_CLASS_DRUID
};

// ============================================================
// Helper: class name string
// ============================================================

std::string GetClassName(uint8 classId)
{
    switch (classId)
    {
        case WOW_CLASS_WARRIOR:      return "Warrior";
        case WOW_CLASS_PALADIN:      return "Paladin";
        case WOW_CLASS_HUNTER:       return "Hunter";
        case WOW_CLASS_ROGUE:        return "Rogue";
        case WOW_CLASS_PRIEST:       return "Priest";
        case WOW_CLASS_DEATH_KNIGHT: return "Death Knight";
        case WOW_CLASS_SHAMAN:       return "Shaman";
        case WOW_CLASS_MAGE:         return "Mage";
        case WOW_CLASS_WARLOCK:      return "Warlock";
        case WOW_CLASS_DRUID:        return "Druid";
        default:                     return "Unknown";
    }
}

// ============================================================
// Helper: validate class ID
// ============================================================

bool IsValidClass(uint8 classId)
{
    for (uint8 c : ALL_CLASSES)
        if (c == classId)
            return true;
    return false;
}

// ============================================================
// Struct: holds a player's three-class state from the DB
// ============================================================

struct MulticlassData
{
    uint8 class1 = 0;
    uint8 class2 = 0;
    uint8 class3 = 0;
    uint8 step   = 0;    // 0 = needs class2, 1 = needs class3, 2 = complete
    bool  exists = false;
};

MulticlassData LoadMulticlassData(uint32 guid)
{
    MulticlassData data;
    QueryResult result = CharacterDatabase.Query(
        "SELECT class1, class2, class3, selection_step FROM character_multiclass WHERE guid = {}",
        guid
    );
    if (result)
    {
        Field* fields = result->Fetch();
        data.class1 = fields[0].Get<uint8>();
        data.class2 = fields[1].Get<uint8>();
        data.class3 = fields[2].Get<uint8>();
        data.step   = fields[3].Get<uint8>();
        data.exists = true;
    }
    return data;
}

// ============================================================
// Helper: send a chat message to the player
// ============================================================

void Notify(Player* player, const std::string& msg)
{
    ChatHandler(player->GetSession()).SendSysMessage(msg.c_str());
}

// ============================================================
// Core: grant all spells the given class would know at the player's level.
//
// Two sources:
//   1. playercreateinfo_spell_custom — passive/auto spells granted at creation
//      (weapon proficiencies, Melee, Shoot, etc.)
//   2. npc_trainer rows linked to class trainers — every spell a trainer teaches
//      up to the player's current level.
//
// We use HasSpell() to skip anything already known so this is safe to call
// repeatedly (on login, on level-up).
// ============================================================

// skipStartSpells  = true  → skip playercreateinfo_spell_custom (proficiency/skill-teaching spells).
//                            Must be true for secondary/tertiary class grants.
//                            AzerothCore validates character_spell on load and deletes any spell that
//                            "teaches a skill" which is invalid for the player's primary race/class.
//                            This causes armor/weapon proficiency loss → item auto-unequip → item
//                            deletion if there is no bag space at load time.  Trainer spells (actual
//                            class abilities) are not affected by this validation.

// Sanctum signature ability: each chosen class grants ONE iconic ability for free,
// at level 1, regardless of whether the class is primary/secondary/tertiary. Curated
// "iconic but tame" per class (avoids the most broken capstones). Decided 2026-06-22.
// NOTE: base ranks are granted (confident IDs). If a scaling nuke (Pyroblast / Chaos
// Bolt / Starfall / Penance / Stormstrike) feels weak at endgame in testing, bump it
// to its max rank — a one-line ID swap here.
static uint32 GetSignatureSpell(uint8 classId)
{
    switch (classId)
    {
        case WOW_CLASS_WARRIOR:      return 46917; // Titan's Grip (dual-wield 2H, passive)
        case WOW_CLASS_PALADIN:      return 31884; // Avenging Wrath (wings cooldown)
        case WOW_CLASS_HUNTER:       return 19574; // Bestial Wrath
        case WOW_CLASS_ROGUE:        return 36554; // Shadowstep
        case WOW_CLASS_PRIEST:       return 47540; // Penance (R1)
        case WOW_CLASS_DEATH_KNIGHT: return 49576; // Death Grip
        case WOW_CLASS_SHAMAN:       return 17364; // Stormstrike (R1)
        case WOW_CLASS_MAGE:         return 11366; // Pyroblast (R1)
        case WOW_CLASS_WARLOCK:      return 50796; // Chaos Bolt (R1)
        case WOW_CLASS_DRUID:        return 48505; // Starfall (R1)
        default:                     return 0;
    }
}

void GrantClassSpells(Player* player, uint8 classId, bool skipStartSpells = false)
{
    // classmask is a bitmask where bit (classId - 1) represents the class.
    uint32 classMask = (1u << (classId - 1));

    // Sanctum signature ability — one iconic spell per chosen class, free at level 1.
    if (uint32 sig = GetSignatureSpell(classId))
        if (!player->HasSpell(sig))
            player->learnSpell(sig, false);

    // --- Starting / passive spells ---
    // Skipped for secondary/tertiary classes: these are proficiency (skill-teaching) spells that
    // AzerothCore's character validation removes as invalid for the player's primary race/class.
    if (!skipStartSpells)
    {
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
                if (!spellId)
                    continue;
                if (!player->HasSpell(spellId))
                    player->learnSpell(spellId, false);
            } while (startSpells->NextRow());
        }
    }

    // --- Trainer spells up to current level ---
    QueryResult trainerSpells = WorldDatabase.Query(
        "SELECT DISTINCT ts.SpellId "
        "FROM trainer_spell ts "
        "INNER JOIN trainer t ON t.Id = ts.TrainerId "
        "WHERE t.Requirement = {} AND t.Type = 0 "
        "AND ts.SpellId > 0 "
        "AND (ts.ReqLevel = 0 OR ts.ReqLevel <= {})",
        classId, player->GetLevel()
    );
    if (trainerSpells)
    {
        do
        {
            uint32 spellId = (*trainerSpells)[0].Get<uint32>();
            if (!spellId)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
                continue;

            // Gate on DBC BaseLevel: this is the minimum level at which each spell rank is
            // designed to be learned. trainer_spell.ReqLevel is 0 for many spells, which
            // would otherwise cause all ranks to be granted at level 1. BaseLevel=0 means
            // "passive / always available" — always grant those.
            if (info->BaseLevel > 0 && info->BaseLevel > player->GetLevel())
                continue;

            // Skip Rogue dagger-only abilities — Sanctum Rogues use any weapon.
            // EquippedItemClass 2 = weapon; subclass mask bit 15 = dagger.
            if (classId == WOW_CLASS_ROGUE)
            {
                if (info->EquippedItemClass == 2 &&
                    (info->EquippedItemSubClassMask & (1 << 15)) &&        // requires dagger
                    !(info->EquippedItemSubClassMask & ~(1 << 15)))        // ONLY dagger
                    continue;
            }

            if (!player->HasSpell(spellId))
                player->learnSpell(spellId, false);
        } while (trainerSpells->NextRow());
    }

    // --- Warlock: demon summon spells (quest rewards or talent gates, not in trainer table) ---
    // Voidwalker/Succubus/Felhunter are quest rewards at levels 10/20/30.
    // Felguard (30146) is the 51-point Demonology talent — never a quest.
    // Secondary/tertiary Warlocks cannot complete any of these quests or invest
    // that many Demonology points at level 10, so we grant all four here.
    if (classId == WOW_CLASS_WARLOCK && player->GetLevel() >= 10)
    {
        static const uint32 warlockDemonSpells[] = {
            697,    // Summon Voidwalker
            712,    // Summon Succubus
            691,    // Summon Felhunter
            30146,  // Summon Felguard
            0
        };
        for (int i = 0; warlockDemonSpells[i] != 0; ++i)
        {
            if (!player->HasSpell(warlockDemonSpells[i]))
                player->learnSpell(warlockDemonSpells[i], false);
        }
    }

    // --- Death Knight: rank-1 core abilities (Acherus quest rewards, not in trainer) ---
    // The DK trainer only has ranks 2+ for these abilities. Rank 1 of each was
    // taught during the Acherus intro zone which Sanctum bypasses. Staggered by
    // level so secondary/tertiary DKs build their kit progressively.
    if (classId == WOW_CLASS_DEATH_KNIGHT)
    {
        uint8 level = player->GetLevel();
        if (level >= 1)
        {
            if (!player->HasSpell(48263)) player->learnSpell(48263, false); // Blood Presence
            if (!player->HasSpell(48266)) player->learnSpell(48266, false); // Frost Presence
            if (!player->HasSpell(48265)) player->learnSpell(48265, false); // Unholy Presence
            if (!player->HasSpell(45477)) player->learnSpell(45477, false); // Icy Touch R1
            if (!player->HasSpell(45462)) player->learnSpell(45462, false); // Plague Strike R1
            // Sanctum: secondary/tertiary DKs get the same utility a real DK gets from
            // mod-dk-rework (which only fires for primary-class DKs). Death Gate = the
            // Acherus teleport (needed to reach the Runeforge); Runeforging = DK weapon
            // enchants. Neither is in the trainer table, so grant explicitly.
            if (!player->HasSpell(50977)) player->learnSpell(50977, false); // Death Gate (Acherus)
            if (!player->HasSpell(53428)) player->learnSpell(53428, false); // Runeforging (DK enchants)
        }
        if (level >= 4)
            if (!player->HasSpell(45902)) player->learnSpell(45902, false); // Blood Strike R1
        if (level >= 7)
            if (!player->HasSpell(47541)) player->learnSpell(47541, false); // Death Coil R1
        if (level >= 10)
            if (!player->HasSpell(49143)) player->learnSpell(49143, false); // Frost Strike R1
        if (level >= 14)
            if (!player->HasSpell(55050)) player->learnSpell(55050, false); // Heart Strike R1
        if (level >= 17)
            if (!player->HasSpell(55090)) player->learnSpell(55090, false); // Scourge Strike R1
        if (level >= 20)
            if (!player->HasSpell(49158)) player->learnSpell(49158, false); // Corpse Explosion R1
    }

    // --- Hunter: Auto Shot + pet system spells ---
    // Auto Shot (75) is a level-1 trainer spell that all Hunters learn at creation.
    // Secondary/tertiary Hunters skip character creation, so grant it explicitly.
    if (classId == WOW_CLASS_HUNTER)
    {
        if (!player->HasSpell(75))
            player->learnSpell(75, false);
    }

    // Tame Beast and pet management spells are gated behind a level-10 quest chain
    // that secondary/tertiary Hunters can never complete, so grant them here.
    if (classId == WOW_CLASS_HUNTER && player->GetLevel() >= 10)
    {
        static const uint32 hunterPetSpells[] = {
            1515,   // Tame Beast
            883,    // Call Pet
            2641,   // Dismiss Pet
            6991,   // Feed Pet
            982,    // Revive Pet
            0
        };
        for (int i = 0; hunterPetSpells[i] != 0; ++i)
        {
            if (!player->HasSpell(hunterPetSpells[i]))
                player->learnSpell(hunterPetSpells[i], false);
        }
    }

    // --- Utility pet classes: auto-grant summon spell at level 10 ---
    // Druid (Force of Nature), Shaman (Feral Spirit), Priest (Shadowfiend), and
    // Mage (Water Elemental) all have permanent utility pets in Sanctum.  These
    // abilities are normally locked behind deep talent points or high-level
    // trainer requirements, which secondary/tertiary characters can never meet.
    // Granting them at level 10 makes the add-on pet accessible to everyone.
    //
    // Mage also receives spell 70937 (Glyph of Eternal Water) — AzerothCore has
    // built-in logic that makes the Water Elemental permanent when the player
    // has this spell in their spellbook.
    if (player->GetLevel() >= 10)
    {
        switch (classId)
        {
            case WOW_CLASS_DRUID:
                // Force of Nature (33831) — 30-point Balance talent, never in trainer
                if (!player->HasSpell(33831)) player->learnSpell(33831, false);
                break;

            case WOW_CLASS_SHAMAN:
                // Feral Spirit (51533) — 50-point Enhancement talent, never in trainer
                if (!player->HasSpell(51533)) player->learnSpell(51533, false);
                break;

            case WOW_CLASS_PRIEST:
                // Shadowfiend (34433) — in trainer at level 66; override to level 10
                if (!player->HasSpell(34433)) player->learnSpell(34433, false);
                break;

            case WOW_CLASS_MAGE:
                // Summon Water Elemental (31687) — not in trainer
                if (!player->HasSpell(31687)) player->learnSpell(31687, false);
                // Glyph of Eternal Water (70937) — makes WE permanent in AC core
                if (!player->HasSpell(70937)) player->learnSpell(70937, false);
                break;

            default:
                break;
        }
    }
}

// ============================================================
// Secondary power cache + refill
//
// WoW players have one primary power type set at class creation.
// Secondary Warriors and Rogues need Rage / Energy to cast their
// abilities, but those pools sit at zero with no regeneration.
//
// The cache stores each online player's multiclass data so the
// WorldScript tick can refill without a DB round-trip every 500ms.
// ============================================================

static std::unordered_map<uint32, MulticlassData> s_multiclassCache;

// Maps TalentTabID → sorted list of TalentIDs in that tab.
// Built once at startup in MulticlassWorldScript::OnStartup via BuildTalentCache().
static std::unordered_map<uint32, std::vector<uint32>> s_tabTalents;

static void CacheMulticlassData(uint32 guid, const MulticlassData& data)
{
    s_multiclassCache[guid] = data;
}

static void EvictMulticlassCache(uint32 guid)
{
    s_multiclassCache.erase(guid);
}

// Called every 500ms per online player from MulticlassWorldScript::OnUpdate.
static void RefillSecondaryPower(Player* player)
{
    uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
    auto it = s_multiclassCache.find(guid);
    if (it == s_multiclassCache.end())
        return;

    const MulticlassData& data = it->second;
    if (!data.exists)
        return;

    // Secondary / tertiary Warrior — keep Rage pool at max (1000 = 100 rage displayed).
    // Skip if Warrior is the primary class (they generate rage normally in combat).
    if (data.class1 != WOW_CLASS_WARRIOR &&
        (data.class2 == WOW_CLASS_WARRIOR || data.class3 == WOW_CLASS_WARRIOR))
    {
        player->SetPower(POWER_RAGE, 1000);
    }

    // Secondary / tertiary Rogue — keep Energy pool at max (100).
    // Skip if Rogue is the primary class (they regenerate energy normally).
    if (data.class1 != WOW_CLASS_ROGUE &&
        (data.class2 == WOW_CLASS_ROGUE || data.class3 == WOW_CLASS_ROGUE))
    {
        player->SetPower(POWER_ENERGY, 100);
    }

    // Druid in any class slot — bear abilities use Rage, cat abilities use Energy.
    // Primary Druids don't natively have Rage/Energy; secondary Druids have neither.
    // Keep both topped up so client-side resource checks always pass.
    // Skip Rage if Warrior primary (already maxed above), skip Energy if Rogue primary.
    if (data.class1 == WOW_CLASS_DRUID ||
        data.class2 == WOW_CLASS_DRUID ||
        data.class3 == WOW_CLASS_DRUID)
    {
        if (data.class1 != WOW_CLASS_WARRIOR)
            player->SetPower(POWER_RAGE, 1000);
        if (data.class1 != WOW_CLASS_ROGUE)
            player->SetPower(POWER_ENERGY, 100);
    }

    // Secondary / tertiary Death Knight — keep Runic Power at max (1000 = 100 RP).
    // Runic Power decays outside combat, so a one-time OnLogin set isn't enough.
    // This 500ms tick prevents RP from draining to 0 between casts.
    // SetMaxPower is included here in case the pool was reset by a relog.
    if (data.class1 != WOW_CLASS_DEATH_KNIGHT &&
        (data.class2 == WOW_CLASS_DEATH_KNIGHT || data.class3 == WOW_CLASS_DEATH_KNIGHT))
    {
        player->SetMaxPower(POWER_RUNIC_POWER, 1000);
        player->SetPower(POWER_RUNIC_POWER, 1000);
    }
}

// ============================================================
// Permanent self-buff system
//
// Max-rank self-buffs every character would re-cast before combat.
// Applied at login and refreshed every 30 seconds via WorldScript tick.
// Excludes: Bloodlust/Heroism, combat cooldowns, no-duration toggles
// (Paladin auras, Mage Armor, Lightning Shield charges — always managed
// by the player or have no expiry). Hunter aspects are permanent toggles.
// ============================================================

static const std::unordered_map<uint32, std::vector<uint32>> PERMANENT_SELF_BUFFS =
{
    { WOW_CLASS_DEATH_KNIGHT, { 57642 }},           // Horn of Winter R2
    { WOW_CLASS_DRUID,        { 26990 }},           // Mark of the Wild R8
    { WOW_CLASS_MAGE,         { 27126 }},           // Arcane Intellect R6
    { WOW_CLASS_PALADIN,      { 27141, 20217 }},    // Blessing of Might R8, Blessing of Kings
    { WOW_CLASS_PRIEST,       { 25389, 27841, 25431 }}, // PW:Fortitude R7, Divine Spirit R5, Inner Fire R7
    { WOW_CLASS_WARRIOR,      { 47436, 47440 }},    // Battle Shout R9, Commanding Shout R5
    { WOW_CLASS_WARLOCK,      { 58094 }},           // Fel Armor R5
};

// Scans every aura currently on the player. Any spell whose DBC DurationEntry
// was patched to -1 by MakeSelfBuffsPermanent but still has a ticking timer
// (e.g. player manually cast it after the initial application) gets corrected.
// Does NOT auto-apply missing buffs — the player chooses what they cast.
static void CorrectPermanentBuffDurations(Player* player)
{
    if (!player->IsAlive())
        return;

    for (auto const& pair : player->GetAppliedAuras())
    {
        Aura* aura = pair.second ? pair.second->GetBase() : nullptr;
        if (!aura) continue;
        SpellInfo const* info = aura->GetSpellInfo();
        if (!info || !info->DurationEntry) continue;
        // DurationEntry->Duration[0] == -1 means we patched this spell to permanent.
        // If the live aura still has a real duration, correct it now.
        if (info->DurationEntry->Duration[0] == -1 && aura->GetMaxDuration() != -1)
        {
            aura->SetMaxDuration(-1);
            aura->SetDuration(-1);
        }
    }
}

// ============================================================
// Stance / form requirement stripping
//
// Called once from AddSC_mod_multiclass() after all DBC and DB
// data have been loaded into sSpellMgr.
//
// Sets Stances = 0 and StancesNot = 0 on every Warrior, Rogue,
// and Druid spell so those abilities are usable in any stance or
// shapeshift form (including humanoid).  This is a global in-memory
// change — it persists until the server restarts.
// ============================================================

// ============================================================
// Stance / form requirement stripping
//
// Iterates every spell in sSpellMgr by SpellFamilyName — far more
// reliable than DB queries, which miss spells granted outside the
// trainer table (passives, auto-grants, hidden ranks, etc.).
// Targets: Warrior, Rogue, Druid.
// ============================================================

static void StripShapeshiftRequirements()
{
    uint32 stripped = 0;

    auto stripSpell = [&](uint32 spellId)
    {
        if (!spellId) return;
        SpellInfo* info = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(spellId));
        if (!info) return;
        if (info->Stances || info->StancesNot)
        {
            info->Stances    = 0;
            info->StancesNot = 0;
            ++stripped;
        }
    };

    // Pass 1: iterate every spell by family — catches spells not in trainer table
    static const uint32 targetFamilies[] = {
        (uint32)SPELLFAMILY_WARRIOR, (uint32)SPELLFAMILY_ROGUE, (uint32)SPELLFAMILY_DRUID
    };
    uint32 maxId = sSpellMgr->GetSpellInfoStoreSize();
    for (uint32 spellId = 1; spellId < maxId; ++spellId)
    {
        SpellInfo* info = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(spellId));
        if (!info) continue;
        for (uint32 fam : targetFamilies)
            if (info->SpellFamilyName == fam) { stripSpell(spellId); break; }
    }

    // Pass 2: trainer table for all three classes — catches spells whose SpellFamilyName
    // is SPELLFAMILY_GENERIC (0) despite being class abilities (e.g. Warrior Charge).
    static const uint8 trainerClasses[] = { WOW_CLASS_WARRIOR, WOW_CLASS_ROGUE, WOW_CLASS_DRUID };
    for (uint8 classId : trainerClasses)
    {
        QueryResult trainerSpells = WorldDatabase.Query(
            "SELECT DISTINCT ts.SpellId FROM trainer_spell ts "
            "INNER JOIN trainer t ON t.Id = ts.TrainerId "
            "WHERE t.Requirement = {} AND t.Type = 0 AND ts.SpellId > 0", classId);
        if (trainerSpells)
            do { stripSpell((*trainerSpells)[0].Get<uint32>()); } while (trainerSpells->NextRow());

        uint32 classMask = (1u << (classId - 1));
        QueryResult startSpells = WorldDatabase.Query(
            "SELECT DISTINCT Spell FROM playercreateinfo_spell_custom "
            "WHERE (classmask & {}) AND classmask != 0", classMask);
        if (startSpells)
            do { stripSpell((*startSpells)[0].Get<uint32>()); } while (startSpells->NextRow());
    }

    // Pass 3: explicit list of spells whose SpellFamilyName is SPELLFAMILY_GENERIC
    // but still have form requirements that must be cleared.
    // 62606 = Savage Defense — passive proc that requires FORM_BEAR/FORM_DIREBEAR.
    static const uint32 extraSpells[] = { 62606, 0 };
    for (int i = 0; extraSpells[i]; ++i)
        stripSpell(extraSpells[i]);

    LOG_INFO("module",
        "[mod-multiclass] Stripped stance/form requirements from {} spell(s) (Warrior, Rogue, Druid).",
        stripped);
}

// ============================================================
// Shield requirement stripping — Warrior only
//
// Spells like Shield Bash, Shield Block, Shield Slam require a
// shield (ItemClass=4 Armor, SubClassMask bit 6 = 64, or
// InventoryTypeMask bit 14 = INVTYPE_SHIELD).
// Setting EquippedItemClass = -1 removes the equipment gate.
// Uses spell-family iteration to catch every affected spell.
// ============================================================

static void StripShieldRequirements()
{
    constexpr int32 ITEM_CLASS_ARMOR      = 4;
    constexpr int32 ARMOR_SUBCLASS_SHIELD = 64;    // 1 << 6
    constexpr int32 INVTYPE_SHIELD_BIT    = 16384; // 1 << 14

    uint32 stripped = 0;
    uint32 maxId    = sSpellMgr->GetSpellInfoStoreSize();

    for (uint32 spellId = 1; spellId < maxId; ++spellId)
    {
        SpellInfo* info = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(spellId));
        if (!info || info->SpellFamilyName != (uint32)SPELLFAMILY_WARRIOR) continue;

        bool needsShield =
            (info->EquippedItemClass == ITEM_CLASS_ARMOR &&
             (info->EquippedItemSubClassMask & ARMOR_SUBCLASS_SHIELD)) ||
            (info->EquippedItemInventoryTypeMask & INVTYPE_SHIELD_BIT);

        if (needsShield)
        {
            info->EquippedItemClass             = -1;
            info->EquippedItemSubClassMask      = 0;
            info->EquippedItemInventoryTypeMask = 0;
            ++stripped;
        }
    }

    LOG_INFO("module",
        "[mod-multiclass] Removed shield requirements from {} Warrior spell(s).", stripped);
}

// ============================================================
// Warrior weapon requirement stripping
//
// Warrior combat spells (Heroic Strike, Rend, Cleave, etc.) require
// a specific weapon type/subclass.  In Sanctum starter weapons may
// not match the exact subclass the DBC demands (e.g. a 2H sword vs
// "2H sword" subclass bit).  Rather than fight item_template subclass
// matching, we simply zero the SubClassMask and InventoryTypeMask on
// every Warrior spell that requires a weapon (EquippedItemClass == 2).
// EquippedItemClass stays 2 so the "must have SOME weapon" gate remains;
// only the "must be THIS exact weapon type" gate is removed.
// ============================================================

static void StripWeaponRequirements()
{
    constexpr int32 ITEM_CLASS_WEAPON = 2;

    uint32 stripped = 0;
    uint32 maxId    = sSpellMgr->GetSpellInfoStoreSize();

    for (uint32 spellId = 1; spellId < maxId; ++spellId)
    {
        SpellInfo* info = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(spellId));
        if (!info || info->SpellFamilyName != (uint32)SPELLFAMILY_WARRIOR) continue;

        if (info->EquippedItemClass == ITEM_CLASS_WEAPON &&
            (info->EquippedItemSubClassMask != 0 || info->EquippedItemInventoryTypeMask != 0))
        {
            info->EquippedItemSubClassMask      = 0;
            info->EquippedItemInventoryTypeMask = 0;
            ++stripped;
        }
    }

    LOG_INFO("module",
        "[mod-multiclass] Stripped weapon-type requirements from {} Warrior spell(s).", stripped);
}

// ============================================================
// Resource cost stripping
//
// Zeros PowerCost and PowerCostPct on every Warrior spell that
// spends Rage, and every Rogue spell that spends Energy.
// Mana costs on any cross-class ability are left untouched.
// Uses spell-family iteration — no DB queries needed.
// ============================================================

static void StripResourceCosts()
{
    uint32 zeroed = 0;
    uint32 maxId  = sSpellMgr->GetSpellInfoStoreSize();

    for (uint32 spellId = 1; spellId < maxId; ++spellId)
    {
        SpellInfo* info = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(spellId));
        if (!info) continue;

        bool isWarrior = (info->SpellFamilyName == (uint32)SPELLFAMILY_WARRIOR);
        bool isRogue   = (info->SpellFamilyName == (uint32)SPELLFAMILY_ROGUE);
        bool isDruid   = (info->SpellFamilyName == (uint32)SPELLFAMILY_DRUID);
        if (!isWarrior && !isRogue && !isDruid) continue;

        // Warriors use Rage, Rogues use Energy, Druids use Rage (bear) or Energy (cat).
        // Strip both non-mana resource costs from Druids so bear/cat abilities are free.
        bool costStripped = false;
        if (isWarrior && info->PowerType == POWER_RAGE &&
            (info->ManaCost > 0 || info->ManaCostPercentage > 0))
            costStripped = true;
        if (isRogue && info->PowerType == POWER_ENERGY &&
            (info->ManaCost > 0 || info->ManaCostPercentage > 0))
            costStripped = true;
        if (isDruid && (info->PowerType == POWER_RAGE || info->PowerType == POWER_ENERGY) &&
            (info->ManaCost > 0 || info->ManaCostPercentage > 0))
            costStripped = true;

        if (costStripped)
        {
            info->ManaCost            = 0;
            info->ManaCostPercentage  = 0;
            ++zeroed;
        }
    }

    LOG_INFO("module",
        "[mod-multiclass] Zeroed resource costs on {} spell(s) (Warrior rage, Rogue energy, Druid rage/energy).",
        zeroed);
}

// ============================================================
// Permanent self-buff DBC duration patch
//
// Sets DurationEntry to the permanent (-1) DBC entry for every
// spell in PERMANENT_SELF_BUFFS and for all Paladin seals.
// Must run in OnStartup after sSpellMgr is fully loaded.
// This makes AddAura() create the aura with -1 duration from the
// start so the client never shows a ticking timer.
// ============================================================

static void MakeSelfBuffsPermanent()
{
    // Find any SpellDurationEntry with Duration == -1 (permanent).
    // Standard DBC has this at ID 21; we search to be safe.
    // Duration is int32 Duration[3]: [0]=base, [1]=perLevel, [2]=max. -1 = permanent.
    SpellDurationEntry const* permEntry = sSpellDurationStore.LookupEntry(21);
    if (!permEntry || permEntry->Duration[0] != -1)
    {
        for (uint32 i = 0; i < sSpellDurationStore.GetNumRows(); ++i)
        {
            SpellDurationEntry const* e = sSpellDurationStore.LookupEntry(i);
            if (e && e->Duration[0] == -1) { permEntry = e; break; }
        }
    }
    if (!permEntry)
    {
        LOG_WARN("module", "[mod-multiclass] MakeSelfBuffsPermanent: no permanent DBC duration entry found.");
        return;
    }

    uint32 patched = 0;

    auto patch = [&](uint32 spellId)
    {
        if (!spellId) return;
        SpellInfo* info = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(spellId));
        if (info && info->DurationEntry && info->DurationEntry != permEntry)
        {
            info->DurationEntry = permEntry;
            ++patched;
        }
    };

    // Auto-applied class buffs
    for (auto const& [classId, spells] : PERMANENT_SELF_BUFFS)
        for (uint32 spellId : spells)
            patch(spellId);

    // Paladin seals — player casts their chosen seal once, it never expires.
    // Switching seals removes the old one automatically (WoW mutual-exclusion logic).
    static const uint32 SEALS[] = {
        // Seal of Righteousness R1-R9
        20154, 21084, 21085, 21086, 25742, 25744, 25745, 27156, 27157,
        // Seal of Wisdom, Seal of Light, Seal of Justice
        20166, 20165, 20164,
        // Seal of Vengeance (Alliance), Seal of Corruption (Horde)
        31801, 53736,
        // Seal of the Martyr, Seal of Command
        53720, 20375, 27170,
        0
    };
    for (int i = 0; SEALS[i]; ++i)
        patch(SEALS[i]);

    LOG_INFO("module",
        "[mod-multiclass] Patched {} spell duration(s) to permanent (class buffs + seals).", patched);
}

// Forward declaration — SendToAddon is defined in the "Talent addon helpers" section below.
static void SendToAddon(Player* player, const std::string& msg);

// ============================================================
// Spellbook addon data sender
//
// Called at end of OnPlayerLogin. Sends three messages per class:
//   SB_INIT:c1:c2:c3          — which classIds the player has
//   SB_SPELLS:cId:id/lvl,...  — all trainer + special spells (chunked)
//   SB_DONE:cId               — signals end of that class's list
//
// The SanctumSpellbook addon parses these on login to build its
// class tabs. spellId/reqLevel pairs let the client gray spells
// the player hasn't reached the level for yet.
// ============================================================

static void SendSpellbookData(Player* player, const MulticlassData& data)
{
    if (!data.exists) return;

    SendToAddon(player, "SB_INIT:" + std::to_string(data.class1) + ":"
        + std::to_string(data.class2) + ":" + std::to_string(data.class3));

    uint8 classIds[3] = { data.class1, data.class2, data.class3 };

    for (int ci = 0; ci < 3; ++ci)
    {
        uint8 classId = classIds[ci];
        if (!classId) continue;

        std::string chunk  = "SB_SPELLS:" + std::to_string(classId) + ":";
        int          count = 0;

        auto flush = [&]()
        {
            if (count > 0)
            {
                SendToAddon(player, chunk);
                chunk  = "SB_SPELLS:" + std::to_string(classId) + ":";
                count  = 0;
            }
        };

        auto addSpell = [&](uint32 spellId, uint32 reqLevel)
        {
            if (!spellId) return;
            if (count > 0) chunk += ",";
            chunk += std::to_string(spellId) + "/" + std::to_string(reqLevel);
            ++count;
            if (count >= 80) flush();
        };

        // --- trainer spells (all levels, all ranks) ---
        QueryResult res = WorldDatabase.Query(
            "SELECT DISTINCT ts.SpellId, ts.ReqLevel "
            "FROM trainer_spell ts "
            "INNER JOIN trainer t ON t.Id = ts.TrainerId "
            "WHERE t.Requirement = {} AND t.Type = 0 AND ts.SpellId > 0 "
            "ORDER BY ts.SpellId",
            classId);

        if (res)
        {
            do
            {
                uint32 spellId  = (*res)[0].Get<uint32>();
                uint32 reqLevel = (*res)[1].Get<uint32>();
                addSpell(spellId, reqLevel);
            } while (res->NextRow());
        }

        // --- per-class special grants (quest rewards, talent gates, etc.) ---
        if (classId == WOW_CLASS_DEATH_KNIGHT)
        {
            uint32 dk[][2] = {{45477,1},{45462,1},{45902,4},{47541,7},
                              {49143,10},{55050,14},{55090,17},{49158,20},{0,0}};
            for (int i = 0; dk[i][0]; ++i) addSpell(dk[i][0], dk[i][1]);
        }
        else if (classId == WOW_CLASS_HUNTER)
        {
            uint32 h[][2] = {{75,1},{1515,10},{883,10},{2641,10},{6991,10},{982,10},{0,0}};
            for (int i = 0; h[i][0]; ++i) addSpell(h[i][0], h[i][1]);
        }
        else if (classId == WOW_CLASS_WARLOCK)
        {
            uint32 w[][2] = {{697,10},{712,20},{691,30},{30146,10},{0,0}};
            for (int i = 0; w[i][0]; ++i) addSpell(w[i][0], w[i][1]);
        }
        else if (classId == WOW_CLASS_MAGE)
        {
            uint32 m[][2] = {{31687,10},{0,0}};
            for (int i = 0; m[i][0]; ++i) addSpell(m[i][0], m[i][1]);
        }
        else if (classId == WOW_CLASS_DRUID)
        {
            uint32 d[][2] = {{33831,10},{0,0}};
            for (int i = 0; d[i][0]; ++i) addSpell(d[i][0], d[i][1]);
        }
        else if (classId == WOW_CLASS_SHAMAN)
        {
            uint32 s[][2] = {{51533,10},{0,0}};
            for (int i = 0; s[i][0]; ++i) addSpell(s[i][0], s[i][1]);
        }
        else if (classId == WOW_CLASS_PRIEST)
        {
            uint32 p[][2] = {{34433,10},{0,0}};
            for (int i = 0; p[i][0]; ++i) addSpell(p[i][0], p[i][1]);
        }

        flush();
        SendToAddon(player, "SB_DONE:" + std::to_string(classId));
    }
}

// ============================================================
// Talent addon helpers
// ============================================================

// Sends a data packet to the SanctumTalents addon.
// The addon registers a CHAT_MSG_SYSTEM filter for messages starting with "||ST||".
static void SendToAddon(Player* player, const std::string& msg)
{
    std::string fullMsg = "||ST||" + msg;
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL,
        nullptr, nullptr, fullMsg);
    player->GetSession()->SendPacket(&data);
}

// Returns total ranks spent in character_multiclass_talents for a given (guid, classId).
static uint32 GetTalentSpentInClass(uint32 guid, uint8 classId)
{
    if (!classId) return 0;
    QueryResult result = CharacterDatabase.Query(
        "SELECT COALESCE(SUM(`rank`), 0) FROM character_multiclass_talents "
        "WHERE guid = {} AND class_id = {}",
        guid, classId);
    return result ? (*result)[0].Get<uint32>() : 0;
}

// Returns total talent ranks the player has spent in the primary (class1) tree.
// Reads directly from HasTalent so it is accurate regardless of what
// SetFreeTalentPoints was called with — fixing the natFree contamination bug.
static uint32 ComputeClass1Spent(Player* player, uint8 class1)
{
    if (!class1) return 0;
    uint32 classMask = 1u << (class1 - 1);
    uint32 spent = 0;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* t = sTalentStore.LookupEntry(i);
        if (!t) continue;
        TalentTabEntry const* tab = sTalentTabStore.LookupEntry(t->TalentTab);
        if (!tab || !(tab->ClassMask & classMask)) continue;
        for (int r = MAX_TALENT_RANK - 1; r >= 0; --r)
        {
            if (t->RankID[r] && player->HasTalent(t->RankID[r], player->GetActiveSpec()))
            {
                spent += static_cast<uint32>(r + 1);
                break;
            }
        }
    }
    return spent;
}

// ============================================================
// Grant all weapon and armor equip skills at max value.
//
// The server-side equip check (PlayerStorage.cpp CanEquipItem) calls
// GetSkillValue(pItem->GetSkill()) and returns EQUIP_ERR_NO_REQUIRED_PROFICIENCY
// if it returns 0 — even if AddWeaponProficiency has been called.
// The fix is to explicitly add every weapon/armor skill via SetSkill(),
// which both creates the entry if missing AND sets it to max level.
// ============================================================

static void GrantAllEquipSkills(Player* player)
{
    uint32 maxSkill = player->GetMaxSkillValueForLevel();

    // Weapon skills
    static const uint32 weaponSkills[] = {
        43,   // SKILL_SWORDS
        44,   // SKILL_AXES
        45,   // SKILL_BOWS
        46,   // SKILL_GUNS
        54,   // SKILL_MACES
        55,   // SKILL_2H_SWORDS
        136,  // SKILL_STAVES
        160,  // SKILL_2H_MACES
        172,  // SKILL_2H_AXES
        173,  // SKILL_DAGGERS
        176,  // SKILL_THROWN
        226,  // SKILL_CROSSBOWS
        228,  // SKILL_WANDS
        229,  // SKILL_POLEARMS
        473,  // SKILL_FIST_WEAPONS
    };
    for (uint32 skill : weaponSkills)
        player->SetSkill(skill, 0, maxSkill, maxSkill);

    // Armor skills
    static const uint32 armorSkills[] = {
        293,  // SKILL_PLATE_MAIL
        413,  // SKILL_MAIL
        414,  // SKILL_LEATHER
        415,  // SKILL_CLOTH
        433,  // SKILL_SHIELD
    };
    for (uint32 skill : armorSkills)
        player->SetSkill(skill, 0, maxSkill, maxSkill);
}


// ============================================================
// Ranged ammo auto-stock
// Any character with a bow, gun, or crossbow in the ranged slot gets ammo.
// Wands (subclass 19) are excluded — they never need ammo.
// On login, level-up, and ranged weapon equip: top up to 500,000 of the
// best arrow/bullet and call SetAmmo so ranged auto-attack recognises the type.
// Stackable limit on 41165/41164 is raised to 999999 via SQL update.
// ============================================================

// Returns true if the player has a bow, gun, or crossbow in the ranged slot.
static bool PlayerHasRangedWeaponEquipped(Player* player)
{
    Item* ranged = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!ranged) return false;
    uint32 sub = ranged->GetTemplate()->SubClass;
    return sub == 2 || sub == 3 || sub == 18; // bow=2, gun=3, crossbow=18
}

static void RefillRangedAmmo(Player* player)
{
    // Use level-1 usable ammo (no RequiredLevel) so any character can fire immediately.
    // Rough Arrow (2512) and Rough Bullet (2519) are vanilla items with no level req.
    const uint32 AMMO_ARROW  = 2512; // Rough Arrow  — no level requirement
    const uint32 AMMO_BULLET = 2519; // Rough Bullet — no level requirement
    const uint32 FILL_TO     = 500000;

    // Stock only the ammo type the equipped weapon actually uses.
    // Bow (subclass 2) and Crossbow (18) use arrows; Gun (3) uses bullets.
    uint32 ammoEntry = AMMO_ARROW;
    Item* ranged = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (ranged && ranged->GetTemplate()->SubClass == 3)
        ammoEntry = AMMO_BULLET;

    uint32 have = player->GetItemCount(ammoEntry);
    if (have < FILL_TO)
        player->AddItem(ammoEntry, FILL_TO - have);

    player->SetAmmo(ammoEntry);
}

// ============================================================
// Player Script
// Hooks: OnCreate, OnDelete, OnLogin, OnLogout, OnLevelChanged
// ============================================================

class MulticlassPlayerScript : public PlayerScript
{
public:
    MulticlassPlayerScript() : PlayerScript("MulticlassPlayerScript") {}

    // Override IsClass for pet-related contexts only.
    // Sanctum-wide pet design: all class-gated pet checks (Tame Beast, pet talent bonuses,
    // pet ability scaling) recognize all 3 of the player's multiclass classes.
    // Demonology buffs apply to every guardian; Tame Beast works for any char with Hunter.
    // Explicitly excluded: TALENT_POINT_CALC (would corrupt talent trees), INIT, TELEPORT,
    // QUEST, TAXI, SKILL, EQUIP_*, WEAPON_SWAP, GRAVEYARD, and NONE (default/generic).
    Optional<bool> OnPlayerIsClass(Player const* player, Classes playerClass, ClassContext context) override
    {
        // Only intercept pet-relevant contexts
        if (context != CLASS_CONTEXT_PET && context != CLASS_CONTEXT_PET_CHARM)
            return std::nullopt;

        if (player->getClass() == static_cast<uint8>(playerClass))
            return true;

        uint32 guid = player->GetGUID().GetCounter();
        auto it = s_multiclassCache.find(guid);
        if (it != s_multiclassCache.end())
        {
            if (it->second.class2 == static_cast<uint8>(playerClass)) return true;
            if (it->second.class3 == static_cast<uint8>(playerClass)) return true;
        }

        return std::nullopt;
    }

    // Fires when a brand-new character is first created (before their very first login).
    // Seeds the character_multiclass row with their starting class.
    // Uses INSERT (not INSERT IGNORE) so any stale row from a previously deleted
    // character that happened to share this GUID is replaced with a clean slate.
    void OnPlayerCreate(Player* player) override
    {
        uint32 guid     = static_cast<uint32>(player->GetGUID().GetCounter());
        uint8 baseClass = player->getClass();

        CharacterDatabase.Execute(
            "INSERT INTO character_multiclass "
            "(guid, class1, class2, class3, selection_step, zone_chosen) "
            "VALUES ({}, {}, 0, 0, 0, 0) "
            "ON DUPLICATE KEY UPDATE "
            "class1=VALUES(class1), class2=0, class3=0, selection_step=0, zone_chosen=0",
            guid, baseClass
        );

        LOG_INFO("module", "[mod-multiclass] Character '{}' (GUID {}) initialized with primary class {}.",
            player->GetName(), guid, baseClass);
    }

    // Fires when a character is permanently deleted.
    // Removes the character_multiclass row so no data leaks to future characters.
    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        uint32 lowGuid = guid.GetCounter();
        EvictMulticlassCache(lowGuid);
        CharacterDatabase.Execute(
            "DELETE FROM character_multiclass WHERE guid = {}",
            lowGuid
        );
        LOG_INFO("module", "[mod-multiclass] Deleted multiclass data for GUID {}.", lowGuid);
    }

    // Fires every time the player logs in.
    // Re-grants secondary/tertiary class spells so they always have everything.
    // Also reminds them if class selection is incomplete.
    // Populates the secondary-power cache so the WorldScript can refill Rage/Energy.
    void OnPlayerLogin(Player* player) override
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        // Always update the cache so the WorldScript tick has fresh data.
        CacheMulticlassData(guid, data);

        // Safety net: if the row is missing (character existed before module was installed),
        // create it now and prompt the player.
        if (!data.exists)
        {
            CharacterDatabase.Execute(
                "INSERT IGNORE INTO character_multiclass "
                "(guid, class1, class2, class3, selection_step) "
                "VALUES ({}, {}, 0, 0, 0)",
                guid, player->getClass()
            );
            Notify(player, "|cffFF8000[Sanctum]|r Find the Class Weaver NPC to choose your two additional classes.");
            return;
        }

        // Grant class1 spells automatically — this is the player's native class.
        // Class2 and class3 spells are granted by the Class Master NPCs.
        if (data.class1)
            GrantClassSpells(player, data.class1, false);

        // Fill secondary power pools immediately so first-cast works at login.
        // NOTE: SetFaction(35) was removed — it made ALL mobs neutral (broke dungeons).
        // City guard NPCs are neutralised via SQL (mod_sanctum_warden_guards_neutral.sql)
        // so cross-faction city access works without disabling dungeon hostility.
        RefillSecondaryPower(player);

        // Grant all weapon and armor proficiencies — Sanctum has no gear restrictions.
        // AddWeaponProficiency / AddArmorProficiency set the bitmask; SendProficiency
        // pushes the update to the client so the equip UI reflects it immediately.
        player->AddWeaponProficiency(0xFFFFFFFF);
        player->AddArmorProficiency(0xFFFFFFFF);
        player->SendProficiency(ITEM_CLASS_WEAPON, 0xFFFFFFFF);
        player->SendProficiency(ITEM_CLASS_ARMOR,  0xFFFFFFFF);

        // Add every weapon and armor skill so CanEquipItem passes for all gear types.
        // UpdateSkillsToMaxSkillsForLevel only raises skills the player ALREADY has;
        // SetSkill adds the entry if missing and sets it to max immediately.
        GrantAllEquipSkills(player);


        // Sync talent points: Sanctum grants 3 per level instead of WoW's native 1.
        // Total earned = 3 * (level - 9), capped at 3 * 71 = 213 at level 80.
        // FreeTalentPoints = totalEarned3x - class1Spent - class2Spent - class3Spent.
        {
            uint32 lvl         = player->GetLevel();
            uint32 natTotal    = (lvl >= 10) ? std::min(static_cast<uint32>(lvl - 9), 71u) : 0u;
            uint32 c1Spent     = ComputeClass1Spent(player, data.class1);
            uint32 spent23     = GetTalentSpentInClass(guid, data.class2)
                               + GetTalentSpentInClass(guid, data.class3);
            uint32 totalEarned = natTotal * 3;
            uint32 totalSpent  = c1Spent + spent23;
            player->SetFreeTalentPoints(totalEarned > totalSpent ? totalEarned - totalSpent : 0u);
        }
        // The initial talent packet fires before OnPlayerLogin — resend so the client
        // shows the 3x Sanctum count instead of the native 1x value it received earlier.
        player->SendTalentsInfoData(false);

        // Send spell lists to the SanctumSpellbook addon.
        SendSpellbookData(player, data);

        // Auto-stock ammo for any character with Hunter as a class.
        if (PlayerHasRangedWeaponEquipped(player))
            RefillRangedAmmo(player);

        // Grant the 4 totem items to any character that has Shaman as a class.
        //
        // Why: Spell::CheckItems() validates totem spells by checking that the player
        // carries item 5175 (Earth), 5176 (Fire), 5177 (Water), and 5178 (Air) in
        // their inventory. In a normal Shaman character these items are granted by
        // playercreateinfo_item at character creation. In Sanctum's multiclass flow,
        // the player is created as a different class so that creation event never fires.
        //
        // Design intent: "Totem items disabled" means players never buy, carry, or
        // worry about these as a resource — NOT that the items don't exist at all.
        // Giving them permanently on every login achieves that: they're always there,
        // they never run out, and the player never has to think about them.
        if (data.class1 == CLASS_SHAMAN || data.class2 == CLASS_SHAMAN || data.class3 == CLASS_SHAMAN)
        {
            static const uint32 TOTEM_ITEMS[] = { 5175, 5176, 5177, 5178 };
            for (uint32 itemId : TOTEM_ITEMS)
            {
                if (!player->HasItemCount(itemId, 1))
                    player->AddItem(itemId, 1);
            }
        }

        // Remind player if they haven't finished selecting.
        if (data.step == 0)
            Notify(player, "|cffFF8000[Sanctum]|r You have not chosen your additional classes. Find the Class Weaver NPC!");
        else if (data.step == 1)
            Notify(player, "|cffFF8000[Sanctum]|r You still need to choose one more class. Find the Class Weaver NPC!");
    }

    // Fires when the player logs out.
    // Removes the player from the secondary-power cache.
    void OnPlayerLogout(Player* player) override
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        EvictMulticlassCache(guid);
    }

    // Fires every time the player gains a level.
    // Grants any newly unlocked spells for secondary/tertiary classes.
    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        // Refresh cache so power refill logic stays accurate after level-up.
        CacheMulticlassData(guid, data);

        // Grant any newly unlocked class1 spells for the new level.
        if (data.class1)
            GrantClassSpells(player, data.class1, false);

        // Re-max all weapon/armor skills on every level-up.
        GrantAllEquipSkills(player);

        // Refill ammo on level-up so hunters never hit empty mid-session.
        if (PlayerHasRangedWeaponEquipped(player))
            RefillRangedAmmo(player);

        // Recompute talent points using the same formula as OnPlayerLogin.
        // InitTalentForLevel (called before this hook) RESETS the free pool to
        // (level-9) - class1Spent — it does not simply "+1". The old "+2 hack"
        // was therefore always wrong above level 10. We recompute from scratch.
        {
            uint32 lvl         = player->GetLevel();
            uint32 natTotal    = (lvl >= 10) ? std::min(static_cast<uint32>(lvl - 9), 71u) : 0u;
            uint32 c1Spent     = ComputeClass1Spent(player, data.class1);
            uint32 spent23     = GetTalentSpentInClass(guid, data.class2)
                               + GetTalentSpentInClass(guid, data.class3);
            uint32 totalEarned = natTotal * 3;
            uint32 totalSpent  = c1Spent + spent23;
            player->SetFreeTalentPoints(totalEarned > totalSpent ? totalEarned - totalSpent : 0u);
        }
        // InitTalentForLevel already pushed the native (1x) count to the client.
        // Resend so the talent UI shows the correct 3x Sanctum value immediately.
        player->SendTalentsInfoData(false);
    }

};

// ============================================================
// World Script
// Refills secondary Rage / Energy pools every 500ms for all
// online players who have Warrior or Rogue as class 2 or 3.
// ============================================================

class MulticlassWorldScript : public WorldScript
{
    uint32 m_timer     = 0;
    uint32 m_buffTimer = 0;

public:
    MulticlassWorldScript() : WorldScript("MulticlassWorldScript") {}

    // Strips SPELL_AURA_MOD_SHAPESHIFT from the five Druid combat-form spells
    // so the server never applies a shapeshift aura when they are cast.
    // The client DBC is patched separately (patch-5.MPQ) to match.
    static void StripDruidForms()
    {
        static const uint32 formSpells[] = { 5487, 9634, 768, 24858, 33891, 0 };
        uint32 stripped = 0;
        for (int i = 0; formSpells[i]; ++i)
        {
            SpellInfo* info = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(formSpells[i]));
            if (!info) continue;
            for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
            {
                if (info->Effects[j].ApplyAuraName == SPELL_AURA_MOD_SHAPESHIFT)
                {
                    info->Effects[j].Effect        = (SpellEffects)0;
                    info->Effects[j].ApplyAuraName = (AuraType)0;
                    info->Effects[j].MiscValue     = 0;
                    ++stripped;
                }
            }
        }
        LOG_INFO("module", "[mod-multiclass] Druid form shapeshifts stripped ({} effects).", stripped);
    }

    // Builds a static map from TalentTabID → list of TalentIDs.
    // Used by the talent tier-requirement check to avoid iterating sTalentStore per cast.
    static void BuildTalentCache()
    {
        s_tabTalents.clear();
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* t = sTalentStore.LookupEntry(i);
            if (!t) continue;
            s_tabTalents[t->TalentTab].push_back(t->TalentID);
        }
        LOG_INFO("module", "[mod-multiclass] Talent cache built ({} tabs).", s_tabTalents.size());
    }

    // OnStartup fires after all DBCs and databases are fully loaded —
    // safe to call sSpellMgr here.
    void OnStartup() override
    {
        StripShapeshiftRequirements();
        StripShieldRequirements();
        StripWeaponRequirements();
        StripResourceCosts();
        StripDruidForms();
        MakeSelfBuffsPermanent();
        BuildTalentCache();
    }

    void OnUpdate(uint32 diff) override
    {
        m_timer += diff;
        if (m_timer < 500)
            return;
        m_timer = 0;

        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            if (!session) continue;
            Player* player = session->GetPlayer();
            if (player && player->IsInWorld())
                RefillSecondaryPower(player);
        }

        m_buffTimer += diff;
        if (m_buffTimer >= 5000)
        {
            m_buffTimer = 0;
            for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
            {
                if (!session) continue;
                Player* player = session->GetPlayer();
                if (player && player->IsInWorld())
                    CorrectPermanentBuffDurations(player);
            }
        }
    }
};

// NOTE: The Class Weaver NPC (npc_class_weaver) has been removed.
// Class selection is now handled entirely by the Sanctum Warden (mod-sanctum-warden).

// ============================================================
// Creature Script — Multiclass Class Trainer
// ScriptName: npc_multiclass_trainer
//
// Assigned via SQL to all class trainer NPCs.
// Shows the standard trainer window for the player's primary class.
// For secondary/tertiary classes matching this trainer, offers an
// instant "Learn all [Class] spells" gossip option instead —
// the native trainer window can't show spells for non-primary classes
// because IsSpellFitByClassAndRace filters by primary class only.
// ============================================================

class MulticlassTrainerScript : public CreatureScript
{
public:
    MulticlassTrainerScript() : CreatureScript("npc_multiclass_trainer") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        // Look up what class this trainer teaches.
        uint8 trainerClass = GetTrainerClass(creature->GetEntry());
        if (!trainerClass)
            return false; // not a class trainer — fall through to default gossip

        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        bool hasThisClass = (data.class1 == trainerClass ||
                             data.class2 == trainerClass ||
                             data.class3 == trainerClass);

        if (!hasThisClass)
        {
            // Player doesn't study this art — show a polite refusal.
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "I do not study this art. Speak with the Sanctum Warden to bind additional classes.",
                GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, GetTrainerTextId(player, creature), creature->GetGUID());
            return true;
        }

        // Primary class → open the standard trainer window.
        if (data.class1 == trainerClass)
        {
            std::string label = "Train " + GetClassName(trainerClass) + " skills.";
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                label.c_str(), GOSSIP_SENDER_MAIN, 1);
        }

        // Secondary or tertiary class → instant grant via gossip.
        if (data.class2 == trainerClass)
        {
            std::string label = "Learn all " + GetClassName(trainerClass) + " spells for my level.";
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                label.c_str(), GOSSIP_SENDER_MAIN, 2);
        }
        if (data.class3 == trainerClass)
        {
            std::string label = "Learn all " + GetClassName(trainerClass) + " spells for my level.";
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                label.c_str(), GOSSIP_SENDER_MAIN, 3);
        }

        SendGossipMenuFor(player, GetTrainerTextId(player, creature), creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        ClearGossipMenuFor(player);

        uint8 trainerClass = GetTrainerClass(creature->GetEntry());

        switch (action)
        {
            case 0:
                // "Not my art" informational item — just close.
                CloseGossipMenuFor(player);
                break;

            case 1:
                // Primary class — close gossip, open standard trainer window.
                CloseGossipMenuFor(player);
                player->GetSession()->SendTrainerList(creature);
                break;

            case 2:
            case 3:
                // Secondary or tertiary class — grant all spells instantly.
                CloseGossipMenuFor(player);
                if (trainerClass)
                {
                    GrantClassSpells(player, trainerClass, true);
                    Notify(player, "|cff00FF00[Sanctum]|r " +
                        GetClassName(trainerClass) + " training complete.");
                }
                break;

            default:
                CloseGossipMenuFor(player);
                break;
        }

        return true;
    }

private:
    // Returns the NPC text ID to display in the gossip window header.
    // Some generic trainers (entries 26324-26332) have gossip_menu_id = 0,
    // causing GetGossipTextId to return DEFAULT_GOSSIP_MESSAGE (0xffffff) —
    // an invalid sentinel that can prevent the window from rendering.
    // Fall back to text ID 1 (blank text, always present) so the window opens.
    static uint32 GetTrainerTextId(Player* player, Creature* creature)
    {
        uint32 textId = player->GetGossipTextId(creature);
        if (textId >= 0xffffff)
            textId = 1;
        return textId;
    }

    // Returns the class ID this trainer teaches, or 0 if not a class trainer.
    // Cached per entry via a simple DB query.
    static uint8 GetTrainerClass(uint32 creatureEntry)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT t.Requirement "
            "FROM creature_default_trainer cdt "
            "INNER JOIN trainer t ON t.Id = cdt.TrainerId "
            "WHERE cdt.CreatureId = {} AND t.Type = 0 AND t.Requirement > 0 "
            "LIMIT 1",
            creatureEntry
        );
        if (result)
            return (*result)[0].Get<uint8>();
        return 0;
    }
};

// ============================================================
// Multiclass Talent Script
//
// Handles talent spending for secondary (class2) and tertiary (class3)
// classes via the SanctumTalents Lua addon.
//
// Communication:
//   Client → Server: SendAddonMessage("SANCTUM_T", "SANCTUM_T:CMD", "PARTY")
//     Arrives as CHAT_MSG_PARTY + LANG_ADDON; intercepted in
//     OnPlayerBeforeSendChatMessage before the party group check.
//     Since the player is solo (no group), the message is silently
//     dropped by AzerothCore after our hook returns.
//
//   Server → Client: CHAT_MSG_SYSTEM prefixed with "||ST||"
//     The addon registers a ChatFrame_AddMessageEventFilter to intercept
//     and suppress these messages before they appear in chat.
//
// Commands handled:
//   INIT              → INIT:c1:c2:c3:freePts
//   GET_CLASS:classId → TAB:cId:tabId:tabpage  (one per tree)
//                        T:tabId:talentId:row:col:maxRank:curRank:depId:depRank:s1:s2:s3:s4:s5
//                        CLASS_DONE:classId
//   LEARN:talentId    → LEARNED:talentId:newRank:newFreePts  or  ERR:message
//   RESET:classId     → RESET_DONE:classId:newFreePts        or  ERR:message
//
// Talent point pool:
//   Total = level - 9 (WoW native rule).
//   The native system tracks class1 spending via m_usedTalentCount and
//   exposes it through GetFreeTalentPoints().
//   Our system tracks class2/class3 spending in character_multiclass_talents
//   and subtracts it from the native free pool on login and level-up so
//   that GetFreeTalentPoints() always reflects the true remaining points.
// ============================================================

class MulticlassTalentScript : public PlayerScript
{
public:
    MulticlassTalentScript() : PlayerScript("MulticlassTalentScript") {}

    // Intercept LANG_ADDON messages from the addon.
    // Lua sends via WHISPER-to-self (PARTY is blocked client-side when solo).
    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        if ((Language)lang != LANG_ADDON) return;
        if (type != CHAT_MSG_PARTY && type != CHAT_MSG_WHISPER) return;

        size_t pos = msg.find("SANCTUM_T:");
        if (pos == std::string::npos) return;

        std::string cmd = msg.substr(pos + 10);

        if (cmd == "INIT")
            HandleInit(player);
        else if (cmd.rfind("GET_CLASS:", 0) == 0)
            HandleGetClass(player, cmd.substr(10));
        else if (cmd.rfind("LEARN:", 0) == 0)
            HandleLearnTalent(player, cmd.substr(6));
        else if (cmd.rfind("RESET:", 0) == 0)
            HandleResetClass(player, cmd.substr(6));
        else if (cmd == "SB_REQUEST")
            HandleSpellbookRequest(player);
        // Message continues to the CHAT_MSG_PARTY switch case.
        // Player has no group → AzerothCore returns silently. No echo to client.
    }

private:
    // --------------------------------------------------------
    // INIT: respond with class IDs and free talent points
    // --------------------------------------------------------
    void HandleInit(Player* player)
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        // Recompute free points here so INIT is always accurate regardless of what
        // happened since login (level-up race, external SetFreeTalentPoints, etc).
        uint32 lvl         = player->GetLevel();
        uint32 natTotal    = (lvl >= 10) ? std::min(static_cast<uint32>(lvl - 9), 71u) : 0u;
        uint32 c1Spent     = ComputeClass1Spent(player, data.class1);
        uint32 spent23     = GetTalentSpentInClass(guid, data.class2)
                           + GetTalentSpentInClass(guid, data.class3);
        uint32 totalEarned = natTotal * 3;
        uint32 totalSpent  = c1Spent + spent23;
        uint32 freePts     = totalEarned > totalSpent ? totalEarned - totalSpent : 0u;
        player->SetFreeTalentPoints(freePts);

        // Protocol: INIT:c1:c2:c3:freePts:totalEarned
        // totalEarned lets the addon compute spent = totalEarned - freePts.
        std::string resp = "INIT:" + std::to_string(data.class1) + ":" +
                                     std::to_string(data.class2) + ":" +
                                     std::to_string(data.class3) + ":" +
                                     std::to_string(freePts) + ":" +
                                     std::to_string(totalEarned);
        SendToAddon(player, resp);
    }

    // --------------------------------------------------------
    // GET_CLASS: send talent tree layout for the requested class
    // --------------------------------------------------------
    void HandleGetClass(Player* player, const std::string& args)
    {
        uint8 classId = static_cast<uint8>(std::stoul(args));
        if (!IsValidClass(classId)) return;

        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        uint32 classMask = (1u << (classId - 1));

        for (uint32 i = 0; i < sTalentTabStore.GetNumRows(); ++i)
        {
            TalentTabEntry const* tab = sTalentTabStore.LookupEntry(i);
            if (!tab) continue;
            if (!(tab->ClassMask & classMask)) continue;
            if (tab->petTalentMask != 0) continue;   // skip pet trees

            // Announce this tree
            SendToAddon(player, "TAB:" + std::to_string(classId) + ":" +
                                        std::to_string(tab->TalentTabID) + ":" +
                                        std::to_string(tab->tabpage));

            // Send each talent in this tree
            MulticlassData mcData = LoadMulticlassData(guid);
            bool isPrimaryClass = (classId == mcData.class1);

            for (uint32 j = 0; j < sTalentStore.GetNumRows(); ++j)
            {
                TalentEntry const* talent = sTalentStore.LookupEntry(j);
                if (!talent || talent->TalentTab != tab->TalentTabID) continue;

                // For primary class, read current rank from the player's known spells.
                // For class2/3, read from our custom DB table.
                uint8 curRank = 0;
                if (isPrimaryClass)
                {
                    for (int r = MAX_TALENT_RANK - 1; r >= 0; --r)
                    {
                        if (talent->RankID[r] && player->HasTalent(talent->RankID[r], player->GetActiveSpec()))
                        {
                            curRank = static_cast<uint8>(r + 1);
                            break;
                        }
                    }
                }
                else
                {
                    QueryResult curRes = CharacterDatabase.Query(
                        "SELECT `rank` FROM character_multiclass_talents WHERE guid = {} AND talent_id = {}",
                        guid, talent->TalentID);
                    curRank = curRes ? (*curRes)[0].Get<uint8>() : 0;
                }

                uint8 maxRank = 0;
                for (int r = MAX_TALENT_RANK - 1; r >= 0; --r)
                    if (talent->RankID[r]) { maxRank = static_cast<uint8>(r + 1); break; }

                std::string tMsg = "T:" + std::to_string(tab->TalentTabID) + ":" +
                                          std::to_string(talent->TalentID) + ":" +
                                          std::to_string(talent->Row) + ":" +
                                          std::to_string(talent->Col) + ":" +
                                          std::to_string(maxRank) + ":" +
                                          std::to_string(curRank) + ":" +
                                          std::to_string(talent->DependsOn) + ":" +
                                          std::to_string(talent->DependsOnRank);

                for (int r = 0; r < MAX_TALENT_RANK; ++r)
                    tMsg += ":" + std::to_string(talent->RankID[r]);

                SendToAddon(player, tMsg);
            }
        }

        SendToAddon(player, "CLASS_DONE:" + std::to_string(classId));
    }

    // --------------------------------------------------------
    // LEARN: apply a talent for any of the player's 3 classes
    // --------------------------------------------------------
    void HandleLearnTalent(Player* player, const std::string& args)
    {
        uint32 talentId;
        try { talentId = std::stoul(args); }
        catch (...) { SendToAddon(player, "ERR:Invalid talent ID"); return; }

        TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
        if (!talent) { SendToAddon(player, "ERR:Unknown talent"); return; }

        TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TalentTab);
        if (!tab) { SendToAddon(player, "ERR:Unknown talent tab"); return; }

        // Identify which class owns this talent
        uint8 talentClass = 0;
        for (uint8 c = 1; c <= 11; ++c)
            if (tab->ClassMask & (1u << (c - 1))) { talentClass = c; break; }

        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        // Must belong to one of the player's 3 chosen classes
        if (talentClass != data.class1 && talentClass != data.class2 && talentClass != data.class3)
        {
            SendToAddon(player, "ERR:Talent does not belong to any of your classes");
            return;
        }

        bool isPrimary = (talentClass == data.class1);

        uint32 freePts = player->GetFreeTalentPoints();
        if (freePts == 0) { SendToAddon(player, "ERR:No talent points available"); return; }


        // Current rank — primary class reads from HasTalent, others from DB
        uint8 curRank = 0;
        if (isPrimary)
        {
            for (int r = MAX_TALENT_RANK - 1; r >= 0; --r)
            {
                if (talent->RankID[r] && player->HasTalent(talent->RankID[r], player->GetActiveSpec()))
                {
                    curRank = static_cast<uint8>(r + 1);
                    break;
                }
            }
        }
        else
        {
            QueryResult curRes = CharacterDatabase.Query(
                "SELECT `rank` FROM character_multiclass_talents WHERE guid = {} AND talent_id = {}",
                guid, talentId);
            curRank = curRes ? (*curRes)[0].Get<uint8>() : 0;
        }

        // Max rank
        uint8 maxRank = 0;
        for (int r = MAX_TALENT_RANK - 1; r >= 0; --r)
            if (talent->RankID[r]) { maxRank = static_cast<uint8>(r + 1); break; }

        if (curRank >= maxRank) { SendToAddon(player, "ERR:Already at max rank"); return; }

        // Prerequisite check
        if (talent->DependsOn != 0)
        {
            uint8 preRank = 0;
            if (isPrimary)
            {
                TalentEntry const* preTalent = sTalentStore.LookupEntry(talent->DependsOn);
                if (preTalent)
                    for (int r = MAX_TALENT_RANK - 1; r >= 0; --r)
                        if (preTalent->RankID[r] && player->HasTalent(preTalent->RankID[r], player->GetActiveSpec()))
                            { preRank = static_cast<uint8>(r + 1); break; }
            }
            else
            {
                QueryResult preRes = CharacterDatabase.Query(
                    "SELECT `rank` FROM character_multiclass_talents WHERE guid = {} AND talent_id = {}",
                    guid, talent->DependsOn);
                preRank = preRes ? (*preRes)[0].Get<uint8>() : 0;
            }
            if (preRank < talent->DependsOnRank)
            {
                SendToAddon(player, "ERR:Prerequisite not met (need rank " +
                    std::to_string(talent->DependsOnRank) + " in required talent)");
                return;
            }
        }

        // Tier requirement: Row * 5 points needed in this tree
        uint32 tierReq = talent->Row * 5;
        if (tierReq > 0)
        {
            auto tabIt = s_tabTalents.find(talent->TalentTab);
            if (tabIt != s_tabTalents.end() && !tabIt->second.empty())
            {
                uint32 tabSpent = 0;
                if (isPrimary)
                {
                    // Sum spent points in this tree from HasTalent checks
                    for (uint32 tid : tabIt->second)
                    {
                        TalentEntry const* t = sTalentStore.LookupEntry(tid);
                        if (!t) continue;
                        for (int r = MAX_TALENT_RANK - 1; r >= 0; --r)
                            if (t->RankID[r] && player->HasTalent(t->RankID[r], player->GetActiveSpec()))
                                { tabSpent += static_cast<uint32>(r + 1); break; }
                    }
                }
                else
                {
                    std::string inClause;
                    for (uint32 tid : tabIt->second)
                    {
                        if (!inClause.empty()) inClause += ",";
                        inClause += std::to_string(tid);
                    }
                    QueryResult tabRes = CharacterDatabase.Query(
                        "SELECT COALESCE(SUM(`rank`), 0) FROM character_multiclass_talents "
                        "WHERE guid = {} AND talent_id IN ({})",
                        guid, inClause);
                    tabSpent = tabRes ? (*tabRes)[0].Get<uint32>() : 0;
                }
                if (tabSpent < tierReq)
                {
                    SendToAddon(player, "ERR:Need " + std::to_string(tierReq) +
                        " points spent in this tree first");
                    return;
                }
            }
        }

        // Apply
        uint8 newRank = curRank + 1;

        if (isPrimary)
        {
            // Use native LearnTalent — properly updates m_usedTalentCount and all WoW tracking.
            // command=true skips AC's own FreeTalentPoints decrement, so we do it manually
            // to keep the shared Sanctum pool in sync and prevent the m_usedTalentCount
            // cascade that causes AC to auto-wipe class1 talents on the next login/level-up.
            player->LearnTalent(talentId, static_cast<uint32>(curRank), true);
            player->SetFreeTalentPoints(freePts > 0 ? freePts - 1 : 0);
        }
        else
        {
            // Secondary/tertiary: manual spell swap + DB record
            uint32 newSpellId = talent->RankID[curRank];
            if (!newSpellId) { SendToAddon(player, "ERR:No spell data for this rank"); return; }

            if (curRank > 0 && talent->RankID[curRank - 1])
                player->removeSpell(talent->RankID[curRank - 1], SPEC_MASK_ALL, false);

            player->learnSpell(newSpellId, false);
            player->SetFreeTalentPoints(freePts - 1);

            CharacterDatabase.Execute(
                "INSERT INTO character_multiclass_talents (guid, class_id, talent_id, `rank`) "
                "VALUES ({}, {}, {}, {}) ON DUPLICATE KEY UPDATE `rank` = {}",
                guid, talentClass, talentId, newRank, newRank);
        }

        SendToAddon(player, "LEARNED:" + std::to_string(talentId) + ":" +
                             std::to_string(newRank) + ":" +
                             std::to_string(player->GetFreeTalentPoints()));
    }

    // --------------------------------------------------------
    // RESET: refund all class2/class3 talents for a class (costs 50g)
    // --------------------------------------------------------
    void HandleResetClass(Player* player, const std::string& args)
    {
        uint8 classId;
        try { classId = static_cast<uint8>(std::stoul(args)); }
        catch (...) { SendToAddon(player, "ERR:Invalid class ID"); return; }

        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        if (classId != data.class2 && classId != data.class3)
        {
            SendToAddon(player, "ERR:Can only reset secondary or tertiary class talents here");
            return;
        }

        const uint32 RESET_COST = 50 * 10000; // 50 gold in copper
        if (player->GetMoney() < RESET_COST)
        {
            SendToAddon(player, "ERR:Requires 50 gold to reset");
            return;
        }

        uint32 totalSpent = GetTalentSpentInClass(guid, classId);

        // Unlearn all talent rank spells for this class
        QueryResult result = CharacterDatabase.Query(
            "SELECT talent_id, `rank` FROM character_multiclass_talents WHERE guid = {} AND class_id = {}",
            guid, classId);
        if (result)
        {
            do {
                uint32 tId  = (*result)[0].Get<uint32>();
                uint8  rank = (*result)[1].Get<uint8>();
                TalentEntry const* t = sTalentStore.LookupEntry(tId);
                if (t && rank > 0 && t->RankID[rank - 1])
                    player->removeSpell(t->RankID[rank - 1], SPEC_MASK_ALL, false);
            } while (result->NextRow());
        }

        CharacterDatabase.Execute(
            "DELETE FROM character_multiclass_talents WHERE guid = {} AND class_id = {}",
            guid, classId);

        player->ModifyMoney(-(int64)RESET_COST);
        uint32 newFree = player->GetFreeTalentPoints() + totalSpent;
        player->SetFreeTalentPoints(newFree);

        SendToAddon(player, "RESET_DONE:" + std::to_string(classId) + ":" + std::to_string(newFree));

        LOG_INFO("module", "[mod-multiclass] Player '{}' reset class {} talents ({} pts refunded, 50g charged).",
            player->GetName(), classId, totalSpent);
    }

    // --------------------------------------------------------
    // SB_REQUEST: client asks for spellbook data on demand.
    // Sent by SanctumSpellbook when it opens with no data —
    // server-push during login arrives before the client is ready.
    // --------------------------------------------------------
    void HandleSpellbookRequest(Player* player)
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);
        SendSpellbookData(player, data);
    }
};

// ============================================================
// Druid Form Suppression
//
// Prevents Bear / Cat / Moonkin / Tree of Life from changing
// the player's model. Player stays humanoid; all form abilities
// remain usable because StripShapeshiftRequirements zeroed all
// stance requirements at startup.
//
// Travel Form, Aquatic Form, and Flight Form are NOT bound here
// and pass through normally.
// ============================================================

class spell_druid_essence_form : public SpellScript
{
    PrepareSpellScript(spell_druid_essence_form);

    void PreventFormEffect(SpellEffIndex effIndex)
    {
        PreventHitEffect(effIndex);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_druid_essence_form::PreventFormEffect, EFFECT_0, SPELL_EFFECT_APPLY_AURA);
        OnEffectHitTarget += SpellEffectFn(spell_druid_essence_form::PreventFormEffect, EFFECT_1, SPELL_EFFECT_APPLY_AURA);
        OnEffectHitTarget += SpellEffectFn(spell_druid_essence_form::PreventFormEffect, EFFECT_2, SPELL_EFFECT_APPLY_AURA);
    }
};

// ============================================================
// .stbl command — server-side totem bar CALL button
// Client sends:  .stbl id1 id2 id3 id4
// Server casts each spell (triggered=true, bypasses GCD) in order.
// Only spells the player actually knows are cast.
// ============================================================

class SanctumTotemBarCommandScript : public CommandScript
{
public:
    SanctumTotemBarCommandScript() : CommandScript("SanctumTotemBarCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable = {
            { "stbl", HandleStblCallCommand, SEC_PLAYER, Console::No },
        };
        return commandTable;
    }

    static bool HandleStblCallCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return true;

        std::istringstream ss(args ? args : "");
        std::string token;
        int count = 0;

        while (std::getline(ss, token, ' ') && count < 4)
        {
            if (token.empty())
                continue;

            bool valid = true;
            for (char c : token)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                {
                    valid = false;
                    break;
                }
            }
            if (!valid)
                continue;

            uint32 spellId = static_cast<uint32>(std::stoul(token));
            if (spellId)
                player->CastSpell(player, spellId, true);

            ++count;
        }

        return true;
    }
};

// ============================================================
// Registration — called by the loader
// ============================================================

void AddSC_mod_multiclass()
{
    new MulticlassPlayerScript();
    new MulticlassWorldScript();
    new MulticlassTrainerScript();
    new MulticlassTalentScript();
    new SanctumTotemBarCommandScript();
    RegisterSpellScript(spell_druid_essence_form);
    LOG_INFO("module", "[mod-multiclass] Module loaded.");
}
