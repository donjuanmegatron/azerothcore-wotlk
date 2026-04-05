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

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "GossipDef.h"
#include "ScriptedGossip.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Log.h"
#include "WorldSession.h"

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
    ChatHandler(player->GetSession()).PSendSysMessage("%s", msg.c_str());
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

void GrantClassSpells(Player* player, uint8 classId)
{
    // classmask is a bitmask where bit (classId - 1) represents the class.
    uint32 classMask = (1u << (classId - 1));

    // --- Starting / passive spells ---
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
    // trainer.Requirement = classId, trainer.Type = 0 (class trainer).
    // trainer_spell links to trainer via TrainerId.
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
            if (spellId && !player->HasSpell(spellId))
                player->learnSpell(spellId, false);
        } while (trainerSpells->NextRow());
    }
}

// ============================================================
// Player Script
// Hooks: OnCreate, OnLogin, OnLevelChanged
// ============================================================

class MulticlassPlayerScript : public PlayerScript
{
public:
    MulticlassPlayerScript() : PlayerScript("MulticlassPlayerScript") {}

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
        CharacterDatabase.Execute(
            "DELETE FROM character_multiclass WHERE guid = {}",
            lowGuid
        );
        LOG_INFO("module", "[mod-multiclass] Deleted multiclass data for GUID {}.", lowGuid);
    }

    // Fires every time the player logs in.
    // Re-grants secondary/tertiary class spells so they always have everything.
    // Also reminds them if class selection is incomplete.
    void OnPlayerLogin(Player* player) override
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

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

        // Re-grant spells for any classes already chosen.
        if (data.class2)
            GrantClassSpells(player, data.class2);
        if (data.class3)
            GrantClassSpells(player, data.class3);

        // Remind player if they haven't finished selecting.
        if (data.step == 0)
            Notify(player, "|cffFF8000[Sanctum]|r You have not chosen your additional classes. Find the Class Weaver NPC!");
        else if (data.step == 1)
            Notify(player, "|cffFF8000[Sanctum]|r You still need to choose one more class. Find the Class Weaver NPC!");
    }

    // Fires every time the player gains a level.
    // Grants any newly unlocked spells for secondary/tertiary classes.
    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        if (data.class2)
            GrantClassSpells(player, data.class2);
        if (data.class3)
            GrantClassSpells(player, data.class3);
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
                    GrantClassSpells(player, trainerClass);
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
// Registration — called by the loader
// ============================================================

void AddSC_mod_multiclass()
{
    new MulticlassPlayerScript();
    new MulticlassTrainerScript();
    LOG_INFO("module", "[mod-multiclass] Module loaded.");
}
