// mod-sanctum-warden.cpp
// Sanctum Warden — New Character Experience NPC
//
// All new characters spawn in Dalaran (set by companion SQL).
// The Sanctum Warden walks every new character through three steps:
//
//   Step 1: Choose your 2nd class  (permanent)
//   Step 2: Choose your 3rd class  (permanent)
//   Step 3: Choose your starting zone and teleport there
//
// Death Knights skip zone selection — they begin their journey in Dalaran.
//
// Requires mod-multiclass: reuses character_multiclass table.
// Companion SQL adds zone_chosen column, spawns this NPC, and sets all
// city guard factions to 35 (neutral to all).
//
// Gossip action layout:
//   1        = open class list for 2nd slot
//   2        = open class list for 3rd slot
//   3        = open zone list
//   100-111  = class name clicked (class = action - 100)
//   200-211  = class confirmed   (class = action - 200, sender = SENDER_CLASS_CONFIRM)
//   300-307  = zone selected     (zone index = action - 300)

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

static const uint32 WARDEN_NPC_ENTRY = 700200;

static const uint32 NPC_TEXT_WARDEN_WELCOME = 700200;
static const uint32 NPC_TEXT_WARDEN_CLASS2  = 700201;
static const uint32 NPC_TEXT_WARDEN_CLASS3  = 700202;
static const uint32 NPC_TEXT_WARDEN_ZONE    = 700203;
static const uint32 NPC_TEXT_WARDEN_CONFIRM = 700204;
static const uint32 NPC_TEXT_WARDEN_DONE    = 700205;

// Sender value for class confirmation items (avoids collision with GOSSIP_SENDER_MAIN = 1).
static const uint32 SENDER_CLASS_CONFIRM = 2;

// ============================================================
// WoW Class IDs (3.3.5a)
// Note: class 10 does not exist in WotLK.
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
// Starting zone table — 8 racial zones, all accessible to any race.
// ============================================================

struct StartZone
{
    const char* name;
    uint32      map;
    float       x, y, z, o;
};

static const StartZone START_ZONES[] =
{
    { "Elwynn Forest (Human)",        0,   -8940.00f,   -131.00f,    83.53f, 0.00f },
    { "Dun Morogh (Dwarf / Gnome)",   0,   -6235.00f,    333.00f,   382.76f, 3.14f },
    { "Teldrassil (Night Elf)",        1,  10313.00f,    834.00f,  1326.41f, 3.14f },
    { "Tirisfal Glades (Undead)",      0,    1680.00f,   1680.00f,   121.67f, 0.00f },
    { "Durotar (Orc / Troll)",         1,    -614.00f,  -4249.00f,    38.72f, 1.57f },
    { "Mulgore (Tauren)",              1,   -2915.00f,   -256.00f,    52.99f, 1.57f },
    { "Azuremyst Isle (Draenei)",    530,   -3959.00f, -13929.00f,   100.61f, 1.57f },
    { "Eversong Woods (Blood Elf)",  530,   10351.00f,  -6355.00f,    33.40f, 3.14f },
};
static const uint8 NUM_ZONES = 8;

// ============================================================
// Helpers
// ============================================================

static std::string GetClassName(uint8 classId)
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

static bool IsValidClass(uint8 classId)
{
    for (uint8 c : ALL_CLASSES)
        if (c == classId) return true;
    return false;
}

static void Notify(Player* player, const std::string& msg)
{
    ChatHandler(player->GetSession()).PSendSysMessage("%s", msg.c_str());
}

// ============================================================
// DB struct — reads character_multiclass including zone_chosen
// ============================================================

struct WardenData
{
    uint8 class1     = 0;
    uint8 class2     = 0;
    uint8 class3     = 0;
    uint8 step       = 0;  // 0=need class2, 1=need class3, 2=classes done
    uint8 zoneChosen = 0;  // 0=needs zone, 1=zone picked (or DK auto-completed)
    bool  exists     = false;
};

static WardenData LoadWardenData(uint32 guid)
{
    WardenData data;
    QueryResult result = CharacterDatabase.Query(
        "SELECT class1, class2, class3, selection_step, zone_chosen "
        "FROM character_multiclass WHERE guid = {}",
        guid
    );
    if (result)
    {
        Field* f        = result->Fetch();
        data.class1     = f[0].Get<uint8>();
        data.class2     = f[1].Get<uint8>();
        data.class3     = f[2].Get<uint8>();
        data.step       = f[3].Get<uint8>();
        data.zoneChosen = f[4].Get<uint8>();
        data.exists     = true;
    }
    return data;
}

// ============================================================
// GrantClassSpells — grants all spells for classId up to player's level.
// Uses the same trainer schema as mod-multiclass and mod-dk-rework.
// ============================================================

static void GrantClassSpells(Player* player, uint8 classId)
{
    uint32 classMask = (1u << (classId - 1));

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
// Creature Script — Sanctum Warden NPC
// ScriptName: npc_sanctum_warden
// ============================================================

class SanctumWardenScript : public CreatureScript
{
public:
    SanctumWardenScript() : CreatureScript("npc_sanctum_warden") {}

    // --------------------------------------------------------
    // OnGossipHello — player right-clicks the Warden.
    // Routes to the correct step in the flow.
    // --------------------------------------------------------
    bool OnGossipHello(Player* player, Creature* creature) override
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        WardenData data = LoadWardenData(guid);
        ClearGossipMenuFor(player);

        // Safety net: row missing (character existed before module was installed)
        if (!data.exists)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "I am not registered with the Sanctum yet. Please relog to fix this.",
                GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_WELCOME, creature->GetGUID());
            return true;
        }

        // All done — show summary
        if (data.step == 2 && data.zoneChosen == 1)
        {
            std::string summary = "Your path is set: " +
                GetClassName(data.class1) + ", " +
                GetClassName(data.class2) + ", and " +
                GetClassName(data.class3) + ". Safe travels, " +
                std::string(player->GetName()) + ".";
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, summary.c_str(), GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_DONE, creature->GetGUID());
            return true;
        }

        // Step 0: Need class 2
        if (data.step == 0)
        {
            AddGossipItemFor(player, GOSSIP_ICON_TALK,
                "I am ready to choose my second class.", GOSSIP_SENDER_MAIN, 1);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_WELCOME, creature->GetGUID());
            return true;
        }

        // Step 1: Need class 3
        if (data.step == 1)
        {
            std::string msg = "Your second class is " + GetClassName(data.class2) +
                ". Now choose your third and final class.";
            AddGossipItemFor(player, GOSSIP_ICON_TALK, msg.c_str(), GOSSIP_SENDER_MAIN, 2);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_CLASS3, creature->GetGUID());
            return true;
        }

        // Step 2: Classes chosen, need zone
        if (data.step == 2 && data.zoneChosen == 0)
        {
            // DKs stay in Dalaran — auto-complete zone selection
            if (player->getClass() == WOW_CLASS_DEATH_KNIGHT)
            {
                CharacterDatabase.Execute(
                    "UPDATE character_multiclass SET zone_chosen = 1 WHERE guid = {}",
                    guid
                );
                Notify(player, "|cffC41F3B[Sanctum]|r Death Knights begin their journey here in Dalaran. You are home.");
                CloseGossipMenuFor(player);
                return true;
            }

            AddGossipItemFor(player, GOSSIP_ICON_TALK,
                "I am ready to choose my starting zone.", GOSSIP_SENDER_MAIN, 3);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_ZONE, creature->GetGUID());
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }

    // --------------------------------------------------------
    // OnGossipSelect — player clicks an item in the gossip window.
    // --------------------------------------------------------
    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        ClearGossipMenuFor(player);
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        WardenData data = LoadWardenData(guid);

        // Open class list for 2nd slot
        if (sender == GOSSIP_SENDER_MAIN && action == 1)
        {
            ShowClassList(player, creature, data, false);
            return true;
        }

        // Open class list for 3rd slot
        if (sender == GOSSIP_SENDER_MAIN && action == 2)
        {
            ShowClassList(player, creature, data, true);
            return true;
        }

        // Open zone list
        if (sender == GOSSIP_SENDER_MAIN && action == 3)
        {
            ShowZoneList(player, creature);
            return true;
        }

        // Class name clicked — show confirmation
        if (sender == GOSSIP_SENDER_MAIN && action >= 100 && action < 200)
        {
            uint8 chosenClass = static_cast<uint8>(action - 100);
            if (!IsValidClass(chosenClass) || chosenClass == data.class1 || chosenClass == data.class2)
            {
                CloseGossipMenuFor(player);
                return true;
            }

            std::string confirmText = "Bind " + GetClassName(chosenClass) +
                " to your soul permanently? This cannot be undone.";
            uint32 backAction = (data.step == 0) ? 1 : 2;

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, confirmText.c_str(),
                SENDER_CLASS_CONFIRM, 200 + chosenClass);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Let me reconsider.",
                GOSSIP_SENDER_MAIN, backAction);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_CONFIRM, creature->GetGUID());
            return true;
        }

        // Class choice confirmed — save and grant spells
        if (sender == SENDER_CLASS_CONFIRM && action >= 200 && action < 300)
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
                Notify(player, "|cffFF8000[Sanctum]|r Return to the Sanctum Warden to choose your third class.");
            }
            else if (data.step == 1)
            {
                CharacterDatabase.Execute(
                    "UPDATE character_multiclass SET class3 = {}, selection_step = 2 WHERE guid = {}",
                    chosenClass, guid
                );
                GrantClassSpells(player, chosenClass);

                // Reload data so the summary message has accurate names
                data = LoadWardenData(guid);
                Notify(player, "|cff00FF00[Sanctum]|r " + GetClassName(chosenClass) + " bound as your third class.");
                Notify(player, "|cff00FF00[Sanctum]|r Identity: " +
                    GetClassName(data.class1) + " / " +
                    GetClassName(data.class2) + " / " +
                    GetClassName(data.class3) + ".");
                Notify(player, "|cffFF8000[Sanctum]|r Return to the Sanctum Warden to choose your starting zone.");
            }

            CloseGossipMenuFor(player);
            return true;
        }

        // Zone selected — teleport player
        if (sender == GOSSIP_SENDER_MAIN && action >= 300 && action < 400)
        {
            uint8 zoneIndex = static_cast<uint8>(action - 300);
            if (zoneIndex >= NUM_ZONES)
            {
                CloseGossipMenuFor(player);
                return true;
            }

            const StartZone& zone = START_ZONES[zoneIndex];

            CharacterDatabase.Execute(
                "UPDATE character_multiclass SET zone_chosen = 1 WHERE guid = {}",
                guid
            );

            Notify(player, "|cff00FF00[Sanctum]|r Sending you to " +
                std::string(zone.name) + ". Your journey begins now.");

            player->TeleportTo(zone.map, zone.x, zone.y, zone.z, zone.o);

            CloseGossipMenuFor(player);
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }

private:
    // Build the class selection list.
    // forThirdSlot = true: also exclude class2 from the list.
    void ShowClassList(Player* player, Creature* creature, const WardenData& data, bool forThirdSlot)
    {
        for (uint8 classId : ALL_CLASSES)
        {
            if (classId == data.class1) continue;
            if (forThirdSlot && classId == data.class2) continue;

            AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                GetClassName(classId).c_str(), GOSSIP_SENDER_MAIN, 100 + classId);
        }

        uint32 textId = forThirdSlot ? NPC_TEXT_WARDEN_CLASS3 : NPC_TEXT_WARDEN_CLASS2;
        SendGossipMenuFor(player, textId, creature->GetGUID());
    }

    // Build the zone selection list.
    void ShowZoneList(Player* player, Creature* creature)
    {
        for (uint8 i = 0; i < NUM_ZONES; ++i)
        {
            AddGossipItemFor(player, GOSSIP_ICON_TAXI,
                START_ZONES[i].name, GOSSIP_SENDER_MAIN, 300 + i);
        }
        SendGossipMenuFor(player, NPC_TEXT_WARDEN_ZONE, creature->GetGUID());
    }
};

// ============================================================
// Registration
// ============================================================

void AddSC_mod_sanctum_warden()
{
    new SanctumWardenScript();
    LOG_INFO("module", "[mod-sanctum-warden] Module loaded. Sanctum Warden ready in Dalaran.");
}
