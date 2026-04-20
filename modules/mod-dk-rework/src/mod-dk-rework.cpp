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
    ChatHandler(player->GetSession()).PSendSysMessage("%s", msg.c_str());
}

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
        if (player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        // Safety net for DKs that existed before this module was installed.
        if (!player->HasSpell(SPELL_DEATH_GATE))
        {
            player->learnSpell(SPELL_DEATH_GATE, false);
            Notify(player, "|cffC41F3B[Sanctum]|r Your Death Knight class has been updated. Welcome to Sanctum.");
        }

        // If this DK is still at the default heroic starting level (55), force to 1.
        // SetLevel() is safe to call here — the player is fully in the world by the
        // time OnPlayerLogin fires, unlike OnPlayerCreate where it broke saves.
        if (player->GetLevel() == 55)
            player->SetLevel(1);

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
        if (player->getClass() != CLASS_DEATH_KNIGHT)
            return;

        ResetRuneCooldowns(player);
    }
};

// ============================================================
// Registration
// ============================================================

void AddSC_mod_dk_rework()
{
    new DKReworkPlayerScript();
    LOG_INFO("module", "[mod-dk-rework] Module loaded. Death Knights start at level 1.");
}
