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
//   sender=1 (GOSSIP_SENDER_MAIN):
//     action 1        = open class list for 2nd slot
//     action 2        = open class list for 3rd slot
//     action 3        = open zone list
//     action 100-111  = class name clicked (class = action - 100)
//     action 300-307  = zone selected (zone index = action - 300)
//   sender=2 (SENDER_CLASS_CONFIRM):
//     action 200-211  = class choice confirmed (class = action - 200)
//   sender=10/11/12 (SENDER_TRAIN_C1/C2/C3):
//     action = page number — show that page of available spells for class slot 1/2/3
//   sender=20/21/22 (SENDER_LEARN_C1/C2/C3):
//     action = spell ID — learn that spell and charge gold

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "GossipDef.h"
#include "ScriptedGossip.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Log.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "DBCStores.h"

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
static const uint32 NPC_TEXT_WARDEN_TRAIN   = 700206;

// Sender value for class confirmation items (avoids collision with GOSSIP_SENDER_MAIN = 1).
static const uint32 SENDER_CLASS_CONFIRM = 2;

// Sender values for spell training pages (one set per class slot).
// SENDER_TRAIN_CX: action = page number — shows that page of available spells.
// SENDER_LEARN_CX: action = spell ID   — learns that specific spell and charges gold.
static const uint32 SENDER_TRAIN_C1 = 10;
static const uint32 SENDER_TRAIN_C2 = 11;
static const uint32 SENDER_TRAIN_C3 = 12;
static const uint32 SENDER_LEARN_C1 = 20;
static const uint32 SENDER_LEARN_C2 = 21;
static const uint32 SENDER_LEARN_C3 = 22;

// Sender for the pet supplies shop.
// action = which item bundle to purchase (1 = Corpse Dust x5, 2 = Soul Shard x5).
static const uint32 SENDER_PET_SHOP = 30;

// ============================================================
// Pet reagent item IDs
// ============================================================

// Corpse Dust: consumed by Raise Dead when no corpse is nearby.
// Without Master of Ghouls talent, DKs need this to summon their ghoul anywhere.
static const uint32 ITEM_CORPSE_DUST = 37201;

// Soul Shard: consumed by Warlock demon-summon spells.
// Normal generation (Drain Soul kills) is disabled in Sanctum, so the Warden
// sells them directly.
static const uint32 ITEM_SOUL_SHARD  = 6265;

// Bundle size and cost for each reagent sold at the Warden.
static const uint32 REAGENT_BUNDLE_SIZE = 5;
static const uint32 REAGENT_COST_COPPER = 500; // 5 silver per bundle of 5

// How many spells to show per gossip page.
static const uint32 SPELLS_PER_PAGE = 20;

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
// GrantClassSpells — grants all TRAINER spells for classId up to player's level.
// Uses the same trainer schema as mod-multiclass and mod-dk-rework.
//
// NOTE: playercreateinfo_spell_custom (starting/proficiency spells) is intentionally
// NOT granted here.  Those spells "teach skills" and AzerothCore's character validation
// (Player::LoadFromDB) deletes any skill-teaching spell that is invalid for the player's
// primary race/class combination on every login.  This caused armor/weapon proficiency
// loss → item auto-unequip → item deletion at load time.  Trainer spells (actual class
// abilities) are not subject to that validation and are safe to grant cross-class.
// ============================================================

// silent = true  → addSpell directly (no notification) — for login/levelup
// silent = false → learnSpell (shows notification) — for first class selection at Warden
static void GrantClassSpells(Player* player, uint8 classId, bool silent = false)
{
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

            // Replicate AzerothCore's CheckSkillLearnedBySpell logic (Player.cpp:3074).
            // Skip any spell linked to a skill line that is invalid for the player's
            // primary race/class — AzerothCore deletes those on login, stripping
            // proficiency → auto-unequip → gear destruction.
            {
                uint32 errorSkill = 0;
                SkillLineAbilityMapBounds skill_bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
                for (auto sla = skill_bounds.first; sla != skill_bounds.second; ++sla)
                {
                    SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(sla->second->SkillLine);
                    if (!pSkill)
                        continue;
                    if (GetSkillRaceClassInfo(pSkill->id, player->getRace(), player->getClass()))
                    {
                        errorSkill = 0;
                        break;
                    }
                    else
                        errorSkill = pSkill->id;
                }
                if (errorSkill)
                    continue;
            }

            if (silent)
                player->addSpell(spellId, SPEC_MASK_ALL, false);
            else if (!player->HasSpell(spellId))
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

        // All done — offer per-class spell training menus + pet supplies
        if (data.step == 2 && data.zoneChosen == 1)
        {
            std::string label1 = "Train " + GetClassName(data.class1) + " spells.";
            std::string label2 = "Train " + GetClassName(data.class2) + " spells.";
            std::string label3 = "Train " + GetClassName(data.class3) + " spells.";
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, label1.c_str(), SENDER_TRAIN_C1, 0);
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, label2.c_str(), SENDER_TRAIN_C2, 0);
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, label3.c_str(), SENDER_TRAIN_C3, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
                "Purchase pet summoning supplies.", SENDER_PET_SHOP, 0);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_TRAIN, creature->GetGUID());
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

        // Spell training — show a page of available spells for a class slot
        if (sender == SENDER_TRAIN_C1 || sender == SENDER_TRAIN_C2 || sender == SENDER_TRAIN_C3)
        {
            uint8  classId    = (sender == SENDER_TRAIN_C1) ? data.class1 :
                                (sender == SENDER_TRAIN_C2) ? data.class2 : data.class3;
            uint32 senderLearn = sender + 10; // SENDER_LEARN_CX = SENDER_TRAIN_CX + 10
            uint32 page        = action;
            ShowSpellPage(player, creature, classId, senderLearn, sender, page);
            return true;
        }

        // Spell training — player clicked a specific spell to learn
        if (sender == SENDER_LEARN_C1 || sender == SENDER_LEARN_C2 || sender == SENDER_LEARN_C3)
        {
            uint8  classId = (sender == SENDER_LEARN_C1) ? data.class1 :
                             (sender == SENDER_LEARN_C2) ? data.class2 : data.class3;
            uint32 spellId = action;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || player->HasSpell(spellId))
            {
                CloseGossipMenuFor(player);
                return true;
            }

            uint32 cost = GetSpellCost(spellId, classId);
            if (cost > 0 && !player->HasEnoughMoney(cost))
            {
                Notify(player, "|cffFF0000[Sanctum]|r You don't have enough gold to learn that spell.");
                CloseGossipMenuFor(player);
                return true;
            }

            if (cost > 0)
                player->ModifyMoney(-(int32)cost);

            player->learnSpell(spellId, false);

            std::string name = (info->SpellName[0]) ? info->SpellName[0] : "Unknown";
            Notify(player, "|cff00FF00[Sanctum]|r Learned: " + name + ".");

            // Reopen spell page so they can continue training
            uint32 senderPage = sender - 10; // SENDER_TRAIN_CX = SENDER_LEARN_CX - 10
            ShowSpellPage(player, creature, classId, sender, senderPage, 0);
            return true;
        }

        // Pet supplies shop — show the menu (action 0) or back button (action 99)
        if (sender == SENDER_PET_SHOP && (action == 0 || action == 99))
        {
            if (action == 99)
                return OnGossipHello(player, creature); // back to main menu
            ShowPetShop(player, creature);
            return true;
        }

        // Pet supplies shop — player clicked a purchase (action 1 or 2)
        if (sender == SENDER_PET_SHOP && action >= 1 && action < 99)
        {
            BuyPetReagent(player, creature, action);
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

            // Grant starter gear before teleporting — only fires here, and this
            // handler is only reachable when zone_chosen == 0 (once per character).
            GrantStarterGear(player, data);

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
    // --------------------------------------------------------
    // Formats a copper amount as "Xg Ys Zc" for display in gossip.
    // --------------------------------------------------------
    static std::string FormatCost(uint32 copper)
    {
        if (copper == 0) return "Free";
        uint32 gold   = copper / 10000;
        uint32 silver = (copper % 10000) / 100;
        uint32 cents  = copper % 100;
        std::string r;
        if (gold)   r += std::to_string(gold)   + "g ";
        if (silver) r += std::to_string(silver) + "s ";
        if (cents)  r += std::to_string(cents)  + "c";
        if (!r.empty() && r.back() == ' ') r.pop_back();
        return r;
    }

    // --------------------------------------------------------
    // Looks up how much a trainer charges for a specific spell.
    // --------------------------------------------------------
    static uint32 GetSpellCost(uint32 spellId, uint8 classId)
    {
        QueryResult result = WorldDatabase.Query(
            "SELECT ts.MoneyCost FROM trainer_spell ts "
            "INNER JOIN trainer t ON t.Id = ts.TrainerId "
            "WHERE ts.SpellId = {} AND t.Requirement = {} AND t.Type = 0 "
            "LIMIT 1",
            spellId, classId
        );
        if (result)
            return (*result)[0].Get<uint32>();
        return 0;
    }

    // --------------------------------------------------------
    // Places a bag item directly into a specific bag slot (19–22) on the player.
    // bagSlot: INVENTORY_SLOT_BAG_START (19) through INVENTORY_SLOT_BAG_START+3 (22).
    // Falls back to AddItem if the slot is already occupied.
    // --------------------------------------------------------
    static void EquipBagToSlot(Player* player, uint32 itemEntry, uint8 bagSlot)
    {
        // If slot already has a bag, fall back to placing it in inventory.
        if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
        {
            player->AddItem(itemEntry, 1);
            return;
        }

        // Build an explicit destination: container=INVENTORY_SLOT_BAG_0, slot=bagSlot.
        ItemPosCountVec dest;
        dest.push_back(ItemPosCount((INVENTORY_SLOT_BAG_0 << 8) | bagSlot, 1));

        Item* bag = player->StoreNewItem(dest, itemEntry, true, 0);
        if (bag)
            player->SendNewItem(bag, 1, true, false);
        else
            player->AddItem(itemEntry, 1); // fallback if store fails
    }

    // --------------------------------------------------------
    // Grants class-appropriate starter gear when a new character picks their zone.
    // Called exactly once per character — zone selection only shows when zone_chosen==0.
    //
    // Each chosen class contributes its own chest + weapon(s).
    // Duplicate chest types across classes are only granted once.
    //   Mail classes:    Warrior Paladin DK Shaman   → 26031 Elekk Rider's Mail
    //   Leather classes: Hunter Rogue Druid           → 24111 Kurken Hide Jerkin
    //   Cloth classes:   Priest Mage Warlock          → 26004 Farmhand's Vest
    //
    // The Sanctum Pack (700401, 50 slots) is granted first so the player has
    // plenty of bag room for all gear items.
    // --------------------------------------------------------
    static void GrantStarterGear(Player* player, const WardenData& data)
    {
        // Grant both starter bags first so the player has room for all gear.
        //   700401 Sanctum Pack    — 50-slot epic bag (gear/general storage)
        //   700400 Reagent Pouch   — 50-slot epic bag (consumables/reagents)
        // Both go into the backpack as items; the player drags them to bag slots to activate.
        static const uint32 SANCTUM_PACK   = 700401;
        static const uint32 REAGENT_POUCH  = 700400;

        struct StarterKit
        {
            uint8  classId;
            uint32 chest;    // armor chest for this class
            uint32 weapon;   // primary weapon (or bow)
            uint32 weapon2;  // secondary weapon (DK only — gives 2 of this)
            uint32 weaponQty;
        };

        // All real WotLK green items (AllowableClass=-1, RequiredLevel=0).
        static const StarterKit KITS[] =
        {
            { WOW_CLASS_WARRIOR,       26031, 27389,     0, 1 }, // Elekk Rider's Mail + Surplus Bastard Sword
            { WOW_CLASS_PALADIN,       26031,  4948,     0, 1 }, // Elekk Rider's Mail + Stinging Mace
            { WOW_CLASS_DEATH_KNIGHT,  26031, 27389, 18957, 1 }, // Elekk Rider's Mail + Bastard Sword + Brushwood Blade x2
            { WOW_CLASS_HUNTER,        24111, 28152,     0, 1 }, // Kurken Hide Jerkin + Quel'Thalas Recurve
            { WOW_CLASS_ROGUE,         24111,  4947,     0, 2 }, // Kurken Hide Jerkin + Jagged Dagger x2
            { WOW_CLASS_SHAMAN,        26031, 26051,     0, 1 }, // Elekk Rider's Mail + 2 Stone Sledgehammer
            { WOW_CLASS_DRUID,         24111,  9603,     0, 1 }, // Kurken Hide Jerkin + Gritroot Staff
            { WOW_CLASS_PRIEST,        26004,  9603,     0, 1 }, // Farmhand's Vest + Gritroot Staff
            { WOW_CLASS_MAGE,          26004,  9603,     0, 1 }, // Farmhand's Vest + Gritroot Staff
            { WOW_CLASS_WARLOCK,       26004,  9603,     0, 1 }, // Farmhand's Vest + Gritroot Staff
        };
        static const uint32 KIT_COUNT = 10;

        uint8 classes[3] = { data.class1, data.class2, data.class3 };

        // Equip both bags directly into bag slots 1 and 2 so they are immediately usable.
        // INVENTORY_SLOT_BAG_START = 19 (slot 1), +1 = 20 (slot 2).
        EquipBagToSlot(player, SANCTUM_PACK,   INVENTORY_SLOT_BAG_START);
        EquipBagToSlot(player, REAGENT_POUCH,  INVENTORY_SLOT_BAG_START + 1);

        // Track which chest entries have already been given to avoid duplicates.
        // (e.g. Warrior + Shaman both get the mail chest — only give one.)
        uint32 grantedChests[3] = {0, 0, 0};
        uint8  numGrantedChests = 0;

        for (uint8 slot = 0; slot < 3; ++slot)
        {
            uint8 classId = classes[slot];
            if (classId == 0)
                continue;

            for (uint32 k = 0; k < KIT_COUNT; ++k)
            {
                if (KITS[k].classId != classId)
                    continue;

                // Chest — give only once per unique entry
                bool alreadyGiven = false;
                for (uint8 c = 0; c < numGrantedChests; ++c)
                    if (grantedChests[c] == KITS[k].chest) { alreadyGiven = true; break; }

                if (!alreadyGiven)
                {
                    player->AddItem(KITS[k].chest, 1);
                    grantedChests[numGrantedChests++] = KITS[k].chest;
                }

                // Weapon(s) — always grant per class
                player->AddItem(KITS[k].weapon, KITS[k].weaponQty);

                if (KITS[k].weapon2 != 0)
                    player->AddItem(KITS[k].weapon2, 2);

                break;
            }
        }

        Notify(player, "|cff00FF00[Sanctum]|r Your starter equipment has been placed in your bags. The Sanctum Pack and Reagent Pouch are equipped and ready.");
    }

    // --------------------------------------------------------
    // Shows the pet summoning supplies shop.
    // Lists Corpse Dust (DK) and Soul Shards (Warlock) with prices.
    // action 1 = Corpse Dust x5, action 2 = Soul Shard x5.
    // --------------------------------------------------------
    void ShowPetShop(Player* player, Creature* creature)
    {
        std::string dustLabel  = "Corpse Dust x"   + std::to_string(REAGENT_BUNDLE_SIZE)
                               + "  (" + FormatCost(REAGENT_COST_COPPER) + ")"
                               + "  [Death Knight: Raise Dead without a corpse]";
        std::string shardLabel = "Soul Shard x"    + std::to_string(REAGENT_BUNDLE_SIZE)
                               + "  (" + FormatCost(REAGENT_COST_COPPER) + ")"
                               + "  [Warlock: demon summons]";

        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, dustLabel.c_str(),  SENDER_PET_SHOP, 1);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, shardLabel.c_str(), SENDER_PET_SHOP, 2);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", SENDER_PET_SHOP, 99);
        SendGossipMenuFor(player, NPC_TEXT_WARDEN_TRAIN, creature->GetGUID());
    }

    // --------------------------------------------------------
    // Handles a pet reagent purchase (action 1 = Corpse Dust, action 2 = Soul Shard).
    // Deducts gold and adds items to inventory.
    // --------------------------------------------------------
    void BuyPetReagent(Player* player, Creature* creature, uint32 action)
    {
        uint32 itemEntry = 0;
        if (action == 1)      itemEntry = ITEM_CORPSE_DUST;
        else if (action == 2) itemEntry = ITEM_SOUL_SHARD;
        else { CloseGossipMenuFor(player); return; }

        if (!player->HasEnoughMoney(static_cast<uint32>(REAGENT_COST_COPPER)))
        {
            Notify(player, "|cffFF0000[Sanctum]|r You don't have enough gold for that.");
            ShowPetShop(player, creature);
            return;
        }

        if (!player->AddItem(itemEntry, REAGENT_BUNDLE_SIZE))
        {
            Notify(player, "|cffFF0000[Sanctum]|r Your inventory is full.");
            ShowPetShop(player, creature);
            return;
        }

        player->ModifyMoney(-static_cast<int32>(REAGENT_COST_COPPER));
        ShowPetShop(player, creature); // reopen shop so they can buy more
    }

    // --------------------------------------------------------
    // Builds a paginated gossip list of available spells for classId.
    // Only shows spells the player hasn't learned yet and can learn
    // at their current level.  Each spell item uses senderLearn so
    // OnGossipSelect knows to execute a purchase.  Prev/Next buttons
    // use senderPage so OnGossipSelect knows to show another page.
    // --------------------------------------------------------
    void ShowSpellPage(Player* player, Creature* creature,
                       uint8 classId, uint32 senderLearn, uint32 senderPage, uint32 page)
    {
        // Collect all learnable spells for this class up to player's level.
        struct SpellEntry { uint32 spellId; uint32 cost; };
        std::vector<SpellEntry> available;

        QueryResult result = WorldDatabase.Query(
            "SELECT DISTINCT ts.SpellId, ts.MoneyCost "
            "FROM trainer_spell ts "
            "INNER JOIN trainer t ON t.Id = ts.TrainerId "
            "WHERE t.Requirement = {} AND t.Type = 0 "
            "AND ts.SpellId > 0 "
            "AND (ts.ReqLevel = 0 OR ts.ReqLevel <= {}) "
            "ORDER BY ts.ReqLevel, ts.SpellId",
            classId, player->GetLevel()
        );
        if (result)
        {
            do
            {
                uint32 spellId = (*result)[0].Get<uint32>();
                uint32 cost    = (*result)[1].Get<uint32>();
                if (player->HasSpell(spellId))
                    continue;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (info)
                    available.push_back({spellId, cost});
            } while (result->NextRow());
        }

        if (available.empty())
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "You have learned all available spells for this class at your level.",
                GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_TRAIN, creature->GetGUID());
            return;
        }

        uint32 total    = static_cast<uint32>(available.size());
        uint32 maxPages = (total + SPELLS_PER_PAGE - 1) / SPELLS_PER_PAGE;
        if (page >= maxPages) page = 0;

        uint32 start = page * SPELLS_PER_PAGE;
        uint32 end   = std::min(start + SPELLS_PER_PAGE, total);

        for (uint32 i = start; i < end; ++i)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(available[i].spellId);
            std::string name = (info && info->SpellName[0]) ? info->SpellName[0] : "Unknown";
            std::string label = name + "  (" + FormatCost(available[i].cost) + ")";
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
                label.c_str(), senderLearn, available[i].spellId);
        }

        if (page > 0)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "<-- Previous page", senderPage, page - 1);
        if (page + 1 < maxPages)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "--> Next page", senderPage, page + 1);

        std::string header = GetClassName(classId) + " spells — page " +
            std::to_string(page + 1) + " of " + std::to_string(maxPages);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, header.c_str(), GOSSIP_SENDER_MAIN, 0);

        SendGossipMenuFor(player, NPC_TEXT_WARDEN_TRAIN, creature->GetGUID());
    }

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
