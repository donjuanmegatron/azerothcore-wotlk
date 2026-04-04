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

// ============================================================
// Constants
// ============================================================

static const uint32 MULTICLASS_NPC_ENTRY = 700100;

// NPC text IDs — these match rows we insert into npc_text in the world SQL.
static const uint32 NPC_TEXT_WELCOME = 700100;
static const uint32 NPC_TEXT_CLASS2  = 700101;
static const uint32 NPC_TEXT_CLASS3  = 700102;
static const uint32 NPC_TEXT_DONE    = 700103;
static const uint32 NPC_TEXT_CONFIRM = 700104;

// Second sender value for confirmation gossip items.
// GOSSIP_SENDER_MAIN (value 1) is used for navigation; we use 2 for confirmations.
static const uint32 SENDER_CONFIRM = 2;

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
    void OnPlayerCreate(Player* player) override
    {
        uint32 guid      = static_cast<uint32>(player->GetGUID().GetCounter());
        uint8 baseClass  = player->getClass();

        CharacterDatabase.Execute(
            "INSERT IGNORE INTO character_multiclass "
            "(guid, class1, class2, class3, selection_step) "
            "VALUES ({}, {}, 0, 0, 0)",
            guid, baseClass
        );

        LOG_INFO("module", "[mod-multiclass] Character '{}' (GUID {}) initialized with primary class {}.",
            player->GetName(), guid, baseClass);
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

        // Grant all spells for any classes already chosen.
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
    // Grants any newly unlocked trainer spells for secondary/tertiary classes.
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

// ============================================================
// Creature Script — Class Weaver NPC
// ScriptName: npc_class_weaver (must match creature_template.ScriptName in the DB)
// ============================================================

class MulticlassCreatureScript : public CreatureScript
{
public:
    MulticlassCreatureScript() : CreatureScript("npc_class_weaver") {}

    // Called when the player right-clicks the NPC to open the gossip window.
    bool OnGossipHello(Player* player, Creature* creature) override
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        ClearGossipMenuFor(player);

        if (data.step == 2)
        {
            // All three classes chosen — show summary and close.
            std::string summary = "Your three classes are bound: " +
                GetClassName(data.class1) + ", " +
                GetClassName(data.class2) + ", and " +
                GetClassName(data.class3) + ". This is a permanent choice. Safe travels.";
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, summary.c_str(), GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, NPC_TEXT_DONE, creature->GetGUID());
            return true;
        }

        if (data.step == 0)
        {
            AddGossipItemFor(player, GOSSIP_ICON_TALK,
                "I am ready to choose my second class.", GOSSIP_SENDER_MAIN, 1);
            SendGossipMenuFor(player, NPC_TEXT_WELCOME, creature->GetGUID());
        }
        else // step == 1
        {
            std::string msg = "Your second class is " + GetClassName(data.class2) +
                ". Now choose your third and final class.";
            AddGossipItemFor(player, GOSSIP_ICON_TALK, msg.c_str(), GOSSIP_SENDER_MAIN, 2);
            SendGossipMenuFor(player, NPC_TEXT_CLASS3, creature->GetGUID());
        }

        return true;
    }

    // Called when the player clicks any item in the gossip menu.
    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        ClearGossipMenuFor(player);

        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        MulticlassData data = LoadMulticlassData(guid);

        // --- Navigation: open the class list ---
        if (sender == GOSSIP_SENDER_MAIN && action == 1)
        {
            ShowClassList(player, creature, data, false);
            return true;
        }
        if (sender == GOSSIP_SENDER_MAIN && action == 2)
        {
            ShowClassList(player, creature, data, true);
            return true;
        }

        // --- Player clicked a class name: show confirmation prompt ---
        if (sender == GOSSIP_SENDER_MAIN && action >= 100 && action < 200)
        {
            uint8 chosenClass = static_cast<uint8>(action - 100);

            // Guard against invalid or duplicate choices.
            if (!IsValidClass(chosenClass) || chosenClass == data.class1 || chosenClass == data.class2)
            {
                CloseGossipMenuFor(player);
                return true;
            }

            std::string confirmText = "Bind " + GetClassName(chosenClass) +
                " to your soul permanently? This cannot be undone.";
            uint32 backAction = (data.step == 0) ? 1 : 2;

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, confirmText.c_str(), SENDER_CONFIRM, 200 + chosenClass);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Let me reconsider.", GOSSIP_SENDER_MAIN, backAction);
            SendGossipMenuFor(player, NPC_TEXT_CONFIRM, creature->GetGUID());
            return true;
        }

        // --- Player confirmed their choice: save to DB and grant spells ---
        if (sender == SENDER_CONFIRM && action >= 200 && action < 300)
        {
            uint8 chosenClass = static_cast<uint8>(action - 200);

            if (!IsValidClass(chosenClass) || chosenClass == data.class1 || chosenClass == data.class2)
            {
                CloseGossipMenuFor(player);
                return true;
            }

            if (data.step == 0)
            {
                CharacterDatabase.Execute(
                    "UPDATE character_multiclass SET class2 = {}, selection_step = 1 WHERE guid = {}",
                    chosenClass, guid
                );
                GrantClassSpells(player, chosenClass);
                Notify(player, "|cff00FF00[Sanctum]|r " + GetClassName(chosenClass) + " bound as your second class.");
                Notify(player, "|cffFF8000[Sanctum]|r Return to the Class Weaver to choose your third class.");
            }
            else if (data.step == 1)
            {
                CharacterDatabase.Execute(
                    "UPDATE character_multiclass SET class3 = {}, selection_step = 2 WHERE guid = {}",
                    chosenClass, guid
                );
                GrantClassSpells(player, chosenClass);

                // Reload for accurate class names in the final message.
                data = LoadMulticlassData(guid);
                Notify(player, "|cff00FF00[Sanctum]|r " + GetClassName(chosenClass) + " bound as your third class.");
                Notify(player, "|cff00FF00[Sanctum]|r Your identity is complete: " +
                    GetClassName(data.class1) + " / " +
                    GetClassName(data.class2) + " / " +
                    GetClassName(data.class3) + ".");
            }

            CloseGossipMenuFor(player);
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }

private:
    // Builds the class selection list gossip page.
    // forThirdSlot = true means we're picking class3, so class2 is also excluded.
    void ShowClassList(Player* player, Creature* creature, const MulticlassData& data, bool forThirdSlot)
    {
        for (uint8 classId : ALL_CLASSES)
        {
            if (classId == data.class1) continue;
            if (forThirdSlot && classId == data.class2) continue;

            AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                GetClassName(classId).c_str(), GOSSIP_SENDER_MAIN, 100 + classId);
        }

        uint32 textId = forThirdSlot ? NPC_TEXT_CLASS3 : NPC_TEXT_CLASS2;
        SendGossipMenuFor(player, textId, creature->GetGUID());
    }
};

// ============================================================
// Registration — called by the loader
// ============================================================

void AddSC_mod_multiclass()
{
    new MulticlassPlayerScript();
    new MulticlassCreatureScript();
    LOG_INFO("module", "[mod-multiclass] Module loaded.");
}
