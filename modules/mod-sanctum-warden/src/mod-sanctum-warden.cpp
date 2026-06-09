// mod-sanctum-warden.cpp
// Sanctum Warden + Class Trainer System
//
// Sanctum Warden (NPC 700200):
//   Handles zone selection for new characters only.
//   Quest chain hub: grants bags/weapons/chests as quest rewards. Sells pet reagents.
//   Class selection has moved to the 10 Class Master NPCs.
//
// Class Master NPCs (entries 700110-700119, one per class):
//   Each master handles:
//     1. Class selection — a player visits to take a second or third class.
//        Dialogue flavour text describes the path, then a confirmation binds them.
//     2. Spell training  — after selection, returns to trainer to learn new spells
//        at any level, one at a time, for the listed gold cost.
//
// Requires mod-multiclass: reuses character_multiclass table.
//
// Gossip layout — Warden (700200):
//   sender=1, action=3        = open zone list
//   sender=1, action=300-307  = zone selected
//   sender=30, action=0/99    = pet shop menu / back
//   sender=30, action=1-2     = purchase reagent
//
// Gossip layout — Class Trainer (700110-700119):
//   sender=1, action=1   = "I wish to walk your path." (interest)
//   sender=1, action=2   = "Yes. Bind me to this path." (confirmed)
//   sender=1, action=0   = close
//   sender=10, action=N  = show page N of available spells
//   sender=20, action=ID = learn spell ID (charges gold)

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
#include "Item.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include <vector>

// ============================================================
// Constants — Warden
// ============================================================

// static const uint32 WARDEN_NPC_ENTRY = 700200; // reserved for future use

static const uint32 NPC_TEXT_WARDEN_WELCOME = 700200;
static const uint32 NPC_TEXT_WARDEN_ZONE    = 700203;
static const uint32 NPC_TEXT_WARDEN_DONE    = 700205;

static const uint32 SENDER_PET_SHOP    = 30;
static const uint32 SENDER_TEST_CHAR   = 60;
static const uint32 SENDER_TEST_CLASS2 = 61;
static const uint32 SENDER_TEST_CLASS3 = 62;

// ============================================================
// Constants — Class Trainers
// ============================================================

static const uint32 TRAINER_WARRIOR      = 700110;
static const uint32 TRAINER_PALADIN      = 700111;
static const uint32 TRAINER_HUNTER       = 700112;
static const uint32 TRAINER_ROGUE        = 700113;
static const uint32 TRAINER_PRIEST       = 700114;
static const uint32 TRAINER_DEATH_KNIGHT = 700115;
static const uint32 TRAINER_SHAMAN       = 700116;
static const uint32 TRAINER_MAGE         = 700117;
static const uint32 TRAINER_WARLOCK      = 700118;
static const uint32 TRAINER_DRUID        = 700119;

// NPC text IDs for each class master (flavor text shown on gossip open)
static const uint32 NPC_TEXT_TRAINER_WARRIOR      = 700210;
static const uint32 NPC_TEXT_TRAINER_PALADIN      = 700211;
static const uint32 NPC_TEXT_TRAINER_HUNTER       = 700212;
static const uint32 NPC_TEXT_TRAINER_ROGUE        = 700213;
static const uint32 NPC_TEXT_TRAINER_PRIEST       = 700214;
static const uint32 NPC_TEXT_TRAINER_DK           = 700215;
static const uint32 NPC_TEXT_TRAINER_SHAMAN       = 700216;
static const uint32 NPC_TEXT_TRAINER_MAGE         = 700217;
static const uint32 NPC_TEXT_TRAINER_WARLOCK      = 700218;
static const uint32 NPC_TEXT_TRAINER_DRUID        = 700219;
static const uint32 NPC_TEXT_TRAINER_CONFIRM      = 700220;
static const uint32 NPC_TEXT_TRAINER_FULL         = 700221;
static const uint32 NPC_TEXT_TRAINER_TRAIN        = 700222;

// Trainer gossip senders
static const uint32 SENDER_TRAINER_PAGE  = 10;
static const uint32 SENDER_TRAINER_LEARN = 20;

// Spells per page in the training list
static const uint32 SPELLS_PER_PAGE = 20;

// Pet reagent items and costs
static const uint32 ITEM_CORPSE_DUST      = 37201;
static const uint32 ITEM_SOUL_SHARD       = 6265;
static const uint32 REAGENT_BUNDLE_SIZE   = 5;
static const uint32 REAGENT_COST_COPPER   = 500;

// ============================================================
// Creation quest chain IDs
// ============================================================
static const uint32 QUEST_WELCOME              = 700001; // bags reward
static const uint32 QUEST_SECOND_PATH          = 700002; // all weapons reward
static const uint32 QUEST_THIRD_PATH           = 700003; // all chests reward
static const uint32 QUEST_WHERE_ADVENTURE      = 700004; // long-chain root

// Gossip senders for quest interactions
static const uint32 SENDER_QUEST_OFFER    = 50; // click to open quest details dialog
static const uint32 SENDER_QUEST_COMPLETE = 51; // click to open quest reward dialog

// Kill-credit dummy NPC entries (never spawned in world)
static const uint32 CREDIT_SECOND_PATH   = 700050; // fires when class2 is bound
static const uint32 CREDIT_THIRD_PATH    = 700051; // fires when class3 is bound
static const uint32 CREDIT_ZONE_CHOSEN   = 700052; // fires when player picks starting zone
static const uint32 CREDIT_LEVEL_10      = 700053; // fires when player reaches level 10

// ============================================================
// WoW Class IDs (3.3.5a)
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

// ALL_CLASSES reserved — used if a future feature needs to iterate all class IDs

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

// ============================================================
// WardenData — per-character class/zone state
// ============================================================

struct WardenData
{
    bool  exists    = false;
    uint8 class1    = 0;
    uint8 class2    = 0;
    uint8 class3    = 0;
    uint8 step      = 0;
    uint8 zoneChosen = 0;
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
        data.exists     = true;
        data.class1     = (*result)[0].Get<uint8>();
        data.class2     = (*result)[1].Get<uint8>();
        data.class3     = (*result)[2].Get<uint8>();
        data.step       = (*result)[3].Get<uint8>();
        data.zoneChosen = (*result)[4].Get<uint8>();
    }
    return data;
}

static void Notify(Player* player, const std::string& msg)
{
    ChatHandler(player->GetSession()).SendSysMessage(msg.c_str());
}

// ============================================================
// GrantClassSpells — grants all trainer spells for classId up
// to the player's current level.
// ============================================================

static void GrantClassSpells(Player* player, uint8 classId)
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
    if (!trainerSpells)
        return;

    do
    {
        uint32 spellId = (*trainerSpells)[0].Get<uint32>();
        if (!spellId)
            continue;

        // Skip spells linked to skill lines invalid for this player's primary class/race.
        // AzerothCore deletes those on login, causing proficiency loss and gear destruction.
        {
            uint32 errorSkill = 0;
            SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
            for (auto sla = bounds.first; sla != bounds.second; ++sla)
            {
                SkillLineEntry const* pSkill = sSkillLineStore.LookupEntry(sla->second->SkillLine);
                if (!pSkill) continue;
                if (GetSkillRaceClassInfo(pSkill->id, player->getRace(), player->getClass()))
                    { errorSkill = 0; break; }
                else
                    errorSkill = pSkill->id;
            }
            if (errorSkill) continue;
        }

        if (!player->HasSpell(spellId))
            player->learnSpell(spellId, false);

    } while (trainerSpells->NextRow());
}

// ============================================================
// Shared trainer helper functions
// ============================================================

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

static uint32 GetSpellCost(uint32 spellId, uint8 classId)
{
    QueryResult result = WorldDatabase.Query(
        "SELECT ts.MoneyCost FROM trainer_spell ts "
        "INNER JOIN trainer t ON t.Id = ts.TrainerId "
        "WHERE ts.SpellId = {} AND t.Requirement = {} AND t.Type = 0 LIMIT 1",
        spellId, classId
    );
    if (result)
        return (*result)[0].Get<uint32>();
    return 0;
}

// Given a trainer_spell SpellId, returns the actual ability the player ends up with.
// Walks SPELL_EFFECT_LEARN_SPELL trigger chains up to 3 hops.
static uint32 ResolveTaughtSpell(uint32 spellId)
{
    for (int hop = 0; hop < 3; ++hop)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info) break;
        bool resolved = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (info->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL &&
                info->Effects[i].TriggerSpell != 0)
            {
                spellId = info->Effects[i].TriggerSpell;
                resolved = true;
                break;
            }
        }
        if (!resolved) break;
    }
    return spellId;
}

// Returns true if the player already knows the ability that trainer row spellId would teach.
// Handles teach-trigger IDs, rank chains, and standalone spells uniformly.
static bool PlayerKnowsCanonical(Player* player, uint32 trainerSpellId)
{
    uint32 taught    = ResolveTaughtSpell(trainerSpellId);
    uint32 candFirst = sSpellMgr->GetFirstSpellInChain(taught);
    if (!candFirst) candFirst = taught;
    uint32 candRank  = sSpellMgr->GetSpellRank(taught);
    if (!candRank) candRank = 1; // treat rank-0 (standalone) as rank 1

    // Quick check on the resolved ID
    if (player->HasSpell(taught)) return true;

    // Walk the player's full spell map
    PlayerSpellMap const& spellMap = player->GetSpellMap();
    for (auto const& kv : spellMap)
    {
        if (kv.second->State == PLAYERSPELL_REMOVED)
            continue;

        uint32 knownFirst = sSpellMgr->GetFirstSpellInChain(kv.first);
        if (!knownFirst) knownFirst = kv.first;

        if (knownFirst != candFirst)
            continue;

        uint32 knownRank = sSpellMgr->GetSpellRank(kv.first);
        if (!knownRank) knownRank = 1;

        if (knownRank >= candRank)
            return true;
    }
    return false;
}

static void ShowSpellPage(Player* player, Creature* creature,
                          uint8 classId, uint32 senderLearn, uint32 senderPage,
                          uint32 page, uint32 npcTextId)
{
    struct SpellEntry { uint32 spellId; uint32 cost; };
    std::vector<SpellEntry> available;

    QueryResult result = WorldDatabase.Query(
        "SELECT DISTINCT ts.SpellId, ts.MoneyCost, ts.ReqLevel "
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

            if (PlayerKnowsCanonical(player, spellId)) continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info) continue;
            available.push_back({spellId, cost});
        } while (result->NextRow());
    }

    if (available.empty())
    {
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            "You have learned everything available to you at your current level.",
            GOSSIP_SENDER_MAIN, 0);
        SendGossipMenuFor(player, npcTextId, creature->GetGUID());
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
        std::string displayName = (info && info->SpellName[0]) ? info->SpellName[0] : "Unknown";
        uint32 rank = sSpellMgr->GetSpellRank(available[i].spellId);
        if (rank > 0)
            displayName += " (Rank " + std::to_string(rank) + ")";
        std::string label = displayName + "  (" + FormatCost(available[i].cost) + ")";
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER,
            label.c_str(), senderLearn, available[i].spellId);
    }

    if (page > 0)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "<-- Previous page", senderPage, page - 1);
    if (page + 1 < maxPages)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "--> Next page",     senderPage, page + 1);

    std::string header = GetClassName(classId) + " — page " +
        std::to_string(page + 1) + " of " + std::to_string(maxPages);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, header.c_str(), GOSSIP_SENDER_MAIN, 0);

    SendGossipMenuFor(player, npcTextId, creature->GetGUID());
}

// ============================================================
// Starting zone data
// ============================================================

struct StartZone
{
    const char* name;
    uint32 map;
    float x, y, z, o;
};

static const uint8 NUM_ZONES = 8;
static const StartZone START_ZONES[NUM_ZONES] =
{
    { "Northshire Valley (Human)",     0,  -8949.95f,  -132.493f,  83.5312f, 0.0f },
    { "Coldridge Valley (Dwarf)",      0,  -6236.29f,   329.38f,  383.49f, 0.62f },
    { "Shadowglen (Night Elf)",        1,  10338.80f,   821.84f, 1326.82f, 1.67f },
    { "Ammen Vale (Draenei)",        530,  -3961.64f,-13931.20f,  100.61f, 2.08f },
    { "Valley of Trials (Orc/Troll)",  1,   -617.52f, -4251.67f,   38.72f, 0.05f },
    { "Deathknell (Undead)",           0,  1676.71f,   1678.31f,  121.67f, 2.70f },
    { "Camp Narache (Tauren)",         1,  -2917.58f,  -257.98f,   52.99f, 0.05f },
    { "Sunstrider Isle (Blood Elf)", 530,  10349.53f,  -6357.29f,   33.43f, 1.57f },
};

// ============================================================
// Class-specific dungeon set gear package
// ============================================================

// ============================================================
// Test character gear sets
// All item IDs verified from acore_world.item_template queries:
//   Quality=3 (blue) or Quality=4 (epic), AllowableClass=-1, RequiredLevel<=60
// ============================================================

// Cross-module API: creates Enchanted/Epic variants for a base item entry.
// Returns true if both are in ObjectMgr now (usable immediately).
// Returns false if just created — server restart needed.
extern bool GearTiers_EnsureVariants(uint32 baseEntry, uint32& outEnchanted, uint32& outEpic);

static const uint32 TEST_BAG_ITEM  = 23162;  // Foror's Crate of Endless Resist Gear Storage (50-slot)
static const uint32 TEST_BAG_COUNT = 4;

// Normal tier base items — blue dungeon items (UBRS/BRD/Strat quality), AllowableClass=-1.
// All item IDs verified from acore_world.item_template (Quality=3, RequiredLevel<=60).
// Enchanted and Epic tiers are created as stat variants of these same items by mod-gear-tiers.
static const uint32 NORMAL_ITEMS[] = {
    12952,  // head  — Gyth's Skull (UBRS)
    11933,  // neck  — Imperial Jewel (BRD)
    12927,  // shldr — Truestrike Shoulders (UBRS)
    13944,  // chest — Tombstone Breastplate
    13950,  // waist — Detention Strap (BRD)
    14522,  // legs  — Maelstrom Leggings
    18507,  // feet  — Boots of the Full Moon
    18754,  // wrist — Fel Hardened Bracers
    18722,  // hands — Death Grips
    18395,  // ring  — Emerald Flame Ring
    11934,  // ring  — Emperor's Seal (BRD)
    18382,  // back  — Fluctuating Cloak
    18370,  // trin  — Vigilance Charm
    11810,  // trin  — Force of Will (BRD)
    13006,  // 1H mace  — Mass of McGowan (UBRS)
    13964,  // 1H sword — Witchblade (Strat)
    13503,  // 1H axe   — Ravager (BRD)
    18389,  // fist     — Claw of the Shadowmancer (Scholo)
    18392,  // dagger   — Distracting Dagger (UBRS)
    13983,  // 2H axe   — Gravestone War Axe
    13163,  // 2H sword — Relentless Scythe (Scholo)
    14024,  // 2H mace  — Hammer of the Grand Crusader (Strat)
    13169,  // polearm  — Pendulum of Doom (ZF)
    18683,  // wand     — The Nicker (Strat)
    13083,  // shield   — Garrett Family Crest (BRD)
    18680,  // bow      — Ancient Bone Bow (UBRS)
};
static constexpr size_t NORMAL_ITEMS_COUNT = sizeof(NORMAL_ITEMS) / sizeof(NORMAL_ITEMS[0]);

// Sends up to 12 items per mail, batching if array has more.
static void MailItemBatch(Player* player, const char* subject, const uint32* items, size_t count)
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    size_t i = 0;
    while (i < count)
    {
        size_t batchEnd = std::min(i + 12, count);
        MailDraft draft(subject, "");
        bool hasItems = false;
        for (size_t j = i; j < batchEnd; ++j)
        {
            Item* item = Item::CreateItem(items[j], 1, player);
            if (item)
            {
                item->SaveToDB(trans);
                draft.AddItem(item);
                hasItems = true;
            }
        }
        if (hasItems)
            draft.SendMailTo(trans, MailReceiver(player),
                MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
        i = batchEnd;
    }
    CharacterDatabase.CommitTransaction(trans);
}

// ============================================================
// Armor sets — 8 slots per class: head, shoulder, chest, waist, legs, feet, wrist, hands
// All items AllowableClass=-1 (no class restriction), RequiredLevel 52-58, Quality=3 (blue).
// Verified from acore_world.item_template.
// Class indices match WoWClass enum. Index 0 and 10 unused (no class 0 or 10 in WoW).
// ============================================================

static const uint32 ARMOR_SET[12][8] =
{
    { 0, 0, 0, 0, 0, 0, 0, 0 },          // [0]  unused
    { 16731, 16733, 16730, 16736, 16732, 16734, 16735, 16737 }, // [1]  Warrior  — Battlegear of Valor (Plate)
    { 16727, 16729, 16726, 16723, 16728, 16725, 16722, 16724 }, // [2]  Paladin  — Lightforge Armor (Plate)
    { 16677, 16679, 16674, 16680, 16678, 16675, 16681, 16676 }, // [3]  Hunter   — Beaststalker Armor (Mail)
    { 16707, 16708, 16721, 16713, 16709, 16711, 16710, 16712 }, // [4]  Rogue    — Shadowcraft Armor (Leather)
    { 16693, 16695, 16690, 16696, 16694, 16691, 16697, 16692 }, // [5]  Priest   — Vestments of the Devout (Cloth)
    { 20551, 19878, 14624, 14620, 14623, 14621, 13951, 14622 }, // [6]  DK       — Dark Plate Assembly (Scholo/Strat/ZG)
    { 16667, 16669, 16666, 16673, 16668, 16670, 16671, 16672 }, // [7]  Shaman   — The Elements (Mail)
    { 16686, 16689, 16688, 16685, 16687, 16682, 16683, 16684 }, // [8]  Mage     — Magister's Regalia (Cloth)
    { 16698, 16701, 16700, 16702, 16699, 16704, 16703, 16705 }, // [9]  Warlock  — Dreadmist Raiment (Cloth)
    { 0, 0, 0, 0, 0, 0, 0, 0 },          // [10] unused
    { 16720, 16718, 16706, 16716, 16719, 16715, 16714, 16717 }, // [11] Druid    — Wildheart Raiment (Leather)
};

// Weapon proficiency bitmask per class. Bit position = weapon item subclass.
// Subclass map: 0=1HAx, 1=2HAx, 4=1HMc, 5=2HMc, 6=Pole, 7=1HSw, 8=2HSw,
//               10=Staff, 13=Fist, 15=Dagr, 19=Wand
// Only weapon types that can appear in a 3-class intersection are relevant here.
// Bow/Gun/Crossbow omitted — at most 2 classes share ranged (Hunter, Rogue),
// so they can never be the intersection of 3 different classes.
static const uint32 WEAPON_PROF[12] =
{
    0,                                                                   // [0]  unused
    (1u<<0)|(1u<<1)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<8)|(1u<<10)|(1u<<13)|(1u<<15), // [1]  Warrior
    (1u<<0)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<8),                   // [2]  Paladin (2H Sword trainable in WotLK)
    (1u<<0)|(1u<<1)|(1u<<6)|(1u<<7)|(1u<<10)|(1u<<13)|(1u<<15),         // [3]  Hunter
    (1u<<0)|(1u<<4)|(1u<<7)|(1u<<13)|(1u<<15),                          // [4]  Rogue
    (1u<<4)|(1u<<10)|(1u<<15)|(1u<<19),                                 // [5]  Priest
    (1u<<0)|(1u<<1)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<7)|(1u<<8),           // [6]  Death Knight
    (1u<<0)|(1u<<1)|(1u<<4)|(1u<<5)|(1u<<10)|(1u<<13)|(1u<<15),        // [7]  Shaman
    (1u<<7)|(1u<<10)|(1u<<15)|(1u<<19),                                 // [8]  Mage
    (1u<<7)|(1u<<10)|(1u<<15)|(1u<<19),                                 // [9]  Warlock
    0,                                                                   // [10] unused
    (1u<<4)|(1u<<5)|(1u<<10)|(1u<<13)|(1u<<15),                        // [11] Druid
};

// Shield proficiency (armor class, tracked separately from weapon subclasses)
static const bool CAN_USE_SHIELD[12] =
{
    false, // [0]  unused
    true,  // [1]  Warrior
    true,  // [2]  Paladin
    false, // [3]  Hunter
    false, // [4]  Rogue
    false, // [5]  Priest
    false, // [6]  Death Knight
    true,  // [7]  Shaman
    false, // [8]  Mage
    false, // [9]  Warlock
    false, // [10] unused
    false, // [11] Druid
};

// One canonical L55-60 blue dungeon item per weapon subclass.
// Entries for subclasses that can never appear in a 3-class intersection are 0.
static const uint32 WEAPON_ITEM_BY_SUBCLASS[20] =
{
    13503, // [0]  1H Axe   — Ravager (BRD)
    13983, // [1]  2H Axe   — Gravestone War Axe
    0,     // [2]  Bow      — never in 3-class intersection
    0,     // [3]  Gun      — never in 3-class intersection
    13006, // [4]  1H Mace  — Mass of McGowan (UBRS)
    14024, // [5]  2H Mace  — Hammer of the Grand Crusader (Strat)
    12583, // [6]  Polearm  — Blackhand Doomsaw (LBRS)
    13964, // [7]  1H Sword — Witchblade (Strat)
    13163, // [8]  2H Sword — Relentless Scythe (Scholo)
    0,     // [9]  obsolete
    18534, // [10] Staff    — Rod of the Ogre Magi (Dire Maul)
    0,     // [11] exotic
    0,     // [12] exotic2
    18389, // [13] Fist     — Claw of the Shadowmancer (Scholo)
    0,     // [14] misc
    18392, // [15] Dagger   — Distracting Dagger (UBRS)
    0,     // [16] Thrown   — never in 3-class intersection
    0,     // [17] obsolete
    0,     // [18] Crossbow — never in 3-class intersection
    18683, // [19] Wand     — The Nicker (Strat)
};

static const uint32 SHIELD_ITEM = 13083; // Garrett Family Crest (BRD)

// Two rings and two trinkets — stat-broadly-useful, AllowableClass=-1, ~L60 blue.
static const uint32 ACCESSORY_ITEMS[] =
{
    18395, // Emerald Flame Ring
    11934, // Emperor's Seal (BRD)
    18370, // Vigilance Charm
    11810, // Force of Will (BRD)
};
static constexpr size_t ACCESSORY_COUNT = sizeof(ACCESSORY_ITEMS) / sizeof(ACCESSORY_ITEMS[0]);

// Returns the full base item list for a 3-class combo: armor sets, shared weapons,
// shield (if all 3 can use), and accessories. Used by both SendStarterGearPackage
// and the test gear menu so they always send identical content.
static std::vector<uint32> BuildClassBaseItems(uint8 c1, uint8 c2, uint8 c3)
{
    std::vector<uint32> baseItems;

    uint8 classes[3] = { c1, c2, c3 };
    for (uint8 cls : classes)
    {
        if (cls == 0 || cls > 11) continue;
        for (int slot = 0; slot < 8; ++slot)
        {
            uint32 id = ARMOR_SET[cls][slot];
            if (id) baseItems.push_back(id);
        }
    }

    bool hasWarrior = (c1 == WOW_CLASS_WARRIOR || c2 == WOW_CLASS_WARRIOR || c3 == WOW_CLASS_WARRIOR);
    auto isTwoHanded = [](uint8 sub) {
        return sub == 1 || sub == 5 || sub == 6 || sub == 8 || sub == 10;
    };
    uint32 sharedWeapons = WEAPON_PROF[c1] & WEAPON_PROF[c2] & WEAPON_PROF[c3];
    for (uint8 sub = 0; sub < 20; ++sub)
    {
        if (!(sharedWeapons & (1u << sub))) continue;
        uint32 id = WEAPON_ITEM_BY_SUBCLASS[sub];
        if (!id) continue;
        baseItems.push_back(id);
        if (hasWarrior && isTwoHanded(sub))
            baseItems.push_back(id);
    }

    if (CAN_USE_SHIELD[c1] && CAN_USE_SHIELD[c2] && CAN_USE_SHIELD[c3])
        baseItems.push_back(SHIELD_ITEM);

    // Ranged weapon for Hunter — bows/guns can never appear in a 3-class weapon intersection,
    // so Hunter's ranged slot is always missing from the shared weapon calculation above.
    // Add explicitly when any of the 3 classes is Hunter.
    bool hasHunter = (c1 == WOW_CLASS_HUNTER || c2 == WOW_CLASS_HUNTER || c3 == WOW_CLASS_HUNTER);
    if (hasHunter)
        baseItems.push_back(18680); // Ancient Bone Bow (UBRS) — enchanted/epic variants auto-generated

    for (size_t i = 0; i < ACCESSORY_COUNT; ++i)
        baseItems.push_back(ACCESSORY_ITEMS[i]);

    return baseItems;
}

// Builds the base item list for a given 3-class combo and sends all 3 gear tiers
// to the player's mailbox. Also grants 10,000 gold. Called once — guarded by gear_sent flag.
// c1/c2/c3 are passed directly to avoid re-querying the DB after an async Execute.
static void SendStarterGearPackage(Player* player, uint8 c1, uint8 c2, uint8 c3)
{
    uint32 guid = player->GetGUID().GetCounter();

    // Guard — never send twice
    QueryResult check = CharacterDatabase.Query(
        "SELECT gear_sent FROM character_multiclass WHERE guid = {}", guid);
    if (check && (*check)[0].Get<uint8>() == 1)
        return;

    std::vector<uint32> baseItems = BuildClassBaseItems(c1, c2, c3);

    if (baseItems.empty())
        return;

    // Resolve Enchanted/Epic variants for every base item
    std::vector<uint32> normalItems, enchItems, epicItems;
    bool needsRestart = false;

    for (uint32 base : baseItems)
    {
        normalItems.push_back(base);
        uint32 ench = 0, epic = 0;
        bool ready = GearTiers_EnsureVariants(base, ench, epic);
        if (ready)
        {
            if (ench) enchItems.push_back(ench);
            if (epic) epicItems.push_back(epic);
        }
        else
            needsRestart = true;
    }

    // Deliver the three tier mails
    MailItemBatch(player, "Your Heritage — Normal Tier",
        normalItems.data(), normalItems.size());
    if (!enchItems.empty())
        MailItemBatch(player, "Your Heritage — Enchanted Tier",
            enchItems.data(), enchItems.size());
    if (!epicItems.empty())
        MailItemBatch(player, "Your Heritage — Epic Tier",
            epicItems.data(), epicItems.size());

    // Gold — 10,000g in copper
    player->ModifyMoney(100000000);

    // Mark sent
    CharacterDatabase.Execute(
        "UPDATE character_multiclass SET gear_sent = 1 WHERE guid = {}", guid);

    if (needsRestart)
        Notify(player,
            "|cffFF8800[Sanctum]|r Your class armor has been delivered (Normal tier). "
            "Restart the server once, then relog to receive Enchanted and Epic tiers.");
    else
        Notify(player,
            "|cff00FF00[Sanctum]|r Your class armor, shared weapons, and 10,000 gold "
            "have been sent to your mailbox in three tiers.");
}

// Adds Sanctum quest chain items to the gossip window as explicit entries.
// PrepareQuestMenu is unreliable when OnGossipHello returns true; this approach
// directly injects offer/complete options that trigger the proper quest dialogs.
//
// Quest chain order: 700001 → 700002 → 700003 → 700004
// Each quest in the chain is only offered once its predecessor has been completed
// (QUEST_STATUS_REWARDED), preventing Q700004 from appearing prematurely before
// the player has bound all three classes and chosen a starting zone.
static void AddWardenQuestItems(Player* player, Creature* creature)
{
    // Q700001 (Welcome / bags): no prerequisites — always available on first visit.
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(QUEST_WELCOME);
        if (quest)
        {
            QuestStatus status = player->GetQuestStatus(QUEST_WELCOME);
            if (status == QUEST_STATUS_NONE && player->CanTakeQuest(quest, false))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    ("[!] " + quest->GetTitle()).c_str(), SENDER_QUEST_OFFER, QUEST_WELCOME);
            else if (status == QUEST_STATUS_COMPLETE)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    ("[?] " + quest->GetTitle()).c_str(), SENDER_QUEST_COMPLETE, QUEST_WELCOME);
        }
    }

    // Q700002 (Second Path / weapons): requires Q700001 rewarded.
    if (player->GetQuestRewardStatus(QUEST_WELCOME))
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(QUEST_SECOND_PATH);
        if (quest)
        {
            QuestStatus status = player->GetQuestStatus(QUEST_SECOND_PATH);
            if (status == QUEST_STATUS_NONE && player->CanTakeQuest(quest, false))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    ("[!] " + quest->GetTitle()).c_str(), SENDER_QUEST_OFFER, QUEST_SECOND_PATH);
            else if (status == QUEST_STATUS_COMPLETE)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    ("[?] " + quest->GetTitle()).c_str(), SENDER_QUEST_COMPLETE, QUEST_SECOND_PATH);
        }
    }

    // Q700003 (Third Path / chests): requires Q700002 rewarded.
    if (player->GetQuestRewardStatus(QUEST_SECOND_PATH))
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(QUEST_THIRD_PATH);
        if (quest)
        {
            QuestStatus status = player->GetQuestStatus(QUEST_THIRD_PATH);
            if (status == QUEST_STATUS_NONE && player->CanTakeQuest(quest, false))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    ("[!] " + quest->GetTitle()).c_str(), SENDER_QUEST_OFFER, QUEST_THIRD_PATH);
            else if (status == QUEST_STATUS_COMPLETE)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    ("[?] " + quest->GetTitle()).c_str(), SENDER_QUEST_COMPLETE, QUEST_THIRD_PATH);
        }
    }

    // Q700004 (Where Adventure Begins): requires Q700003 rewarded.
    // This is intentionally gated last — the player must have bound all three classes
    // (Q700002 + Q700003 objectives) before the adventure-start quest is offered.
    if (player->GetQuestRewardStatus(QUEST_THIRD_PATH))
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(QUEST_WHERE_ADVENTURE);
        if (quest)
        {
            QuestStatus status = player->GetQuestStatus(QUEST_WHERE_ADVENTURE);
            if (status == QUEST_STATUS_NONE && player->CanTakeQuest(quest, false))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    ("[!] " + quest->GetTitle()).c_str(), SENDER_QUEST_OFFER, QUEST_WHERE_ADVENTURE);
            else if (status == QUEST_STATUS_COMPLETE)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                    ("[?] " + quest->GetTitle()).c_str(), SENDER_QUEST_COMPLETE, QUEST_WHERE_ADVENTURE);
        }
    }

    (void)creature; // reserved for future per-NPC quest filtering
}

// ============================================================
// Sanctum Warden NPC — zone selection + pet reagents only
// ============================================================

class SanctumWardenScript : public CreatureScript
{
public:
    SanctumWardenScript() : CreatureScript("npc_sanctum_warden") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        WardenData data = LoadWardenData(guid);
        ClearGossipMenuFor(player);

        // Add quest chain items as explicit gossip entries (PrepareQuestMenu is
        // unreliable when the script returns true from OnGossipHello).
        AddWardenQuestItems(player, creature);

        // Test option always appears first, regardless of character state.
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
            "[Test] Raid Test Setup", SENDER_TEST_CHAR, 0);

        if (!data.exists)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "I am not yet registered with the Sanctum. Please relog.", GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_WELCOME, creature->GetGUID());
            return true;
        }

        // Classes not yet fully chosen — direct them to the Class Masters
        if (data.step < 2)
        {
            uint8 remaining = 2 - data.step;
            std::string msg = "Seek out the Class Masters nearby. You still have " +
                std::to_string(remaining) + " class" +
                (remaining == 1 ? "" : "es") + " to bind.";
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, msg.c_str(), GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_WELCOME, creature->GetGUID());
            return true;
        }

        // Classes done, zone not yet chosen
        if (data.step == 2 && data.zoneChosen == 0)
        {
            AddGossipItemFor(player, GOSSIP_ICON_TAXI,
                "I am ready to choose my starting zone.", GOSSIP_SENDER_MAIN, 3);
            SendGossipMenuFor(player, NPC_TEXT_WARDEN_ZONE, creature->GetGUID());
            return true;
        }

        // Fully set up — show pet shop
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
            "Purchase pet summoning supplies.", SENDER_PET_SHOP, 0);
        SendGossipMenuFor(player, NPC_TEXT_WARDEN_DONE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        ClearGossipMenuFor(player);
        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());

        // Quest offer — player clicked an available quest item; show details dialog
        if (sender == SENDER_QUEST_OFFER)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(action);
            if (quest)
                player->PlayerTalkClass->SendQuestGiverQuestDetails(quest, creature->GetGUID(), true);
            return true;
        }

        // Quest turn-in — player clicked a completeable quest item; show reward dialog
        if (sender == SENDER_QUEST_COMPLETE)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(action);
            if (quest)
                player->PlayerTalkClass->SendQuestGiverOfferReward(quest, creature->GetGUID(), true);
            return true;
        }

        // Open zone list
        if (sender == GOSSIP_SENDER_MAIN && action == 3)
        {
            ShowZoneList(player, creature);
            return true;
        }

        // Zone selected — credit quest objective then teleport
        if (sender == GOSSIP_SENDER_MAIN && action >= 300 && action < 400)
        {
            uint8 zoneIndex = static_cast<uint8>(action - 300);
            if (zoneIndex >= NUM_ZONES) { CloseGossipMenuFor(player); return true; }

            const StartZone& zone = START_ZONES[zoneIndex];
            CharacterDatabase.Execute(
                "UPDATE character_multiclass SET zone_chosen = 1 WHERE guid = {}",
                guid);

            // Credit Quest 700004 objective 1 (choose a starting zone)
            player->KilledMonsterCredit(CREDIT_ZONE_CHOSEN);

            Notify(player, "|cff00FF00[Sanctum]|r Sending you to " +
                std::string(zone.name) + ". Your journey begins.");
            player->TeleportTo(zone.map, zone.x, zone.y, zone.z, zone.o);
            CloseGossipMenuFor(player);
            return true;
        }

        // Pet shop menu or back
        if (sender == SENDER_PET_SHOP && (action == 0 || action == 99))
        {
            if (action == 99) return OnGossipHello(player, creature);
            ShowPetShop(player, creature);
            return true;
        }

        // Pet shop purchase
        if (sender == SENDER_PET_SHOP && action >= 1 && action < 99)
        {
            BuyPetReagent(player, creature, action);
            return true;
        }

        // Test character setup — sequential dialogue flow
        if (sender == SENDER_TEST_CHAR)
        {
            HandleTestChar(player, creature, action);
            return true;
        }

        // Test class pickers — sender 61 = class2, sender 62 = class3
        if (sender == SENDER_TEST_CLASS2 || sender == SENDER_TEST_CLASS3)
        {
            AssignTestClass(player, creature, (sender == SENDER_TEST_CLASS2) ? 2 : 3, static_cast<uint8>(action));
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }

    bool OnQuestReward(Player* /*player*/, Creature* /*creature*/, Quest const* /*quest*/, uint32 /*opt*/) override
    {
        return false; // gear is now sent automatically at 3rd class selection
    }

private:
    void ShowZoneList(Player* player, Creature* creature)
    {
        for (uint8 i = 0; i < NUM_ZONES; ++i)
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, START_ZONES[i].name, GOSSIP_SENDER_MAIN, 300 + i);
        SendGossipMenuFor(player, NPC_TEXT_WARDEN_ZONE, creature->GetGUID());
    }

    void ShowPetShop(Player* player, Creature* creature)
    {
        std::string dust  = "Corpse Dust x5  (" + FormatCost(REAGENT_COST_COPPER) + ")  [Death Knight]";
        std::string shard = "Soul Shard x5  ("  + FormatCost(REAGENT_COST_COPPER) + ")  [Warlock]";
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, dust.c_str(),  SENDER_PET_SHOP, 1);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, shard.c_str(), SENDER_PET_SHOP, 2);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", SENDER_PET_SHOP, 99);
        SendGossipMenuFor(player, NPC_TEXT_WARDEN_DONE, creature->GetGUID());
    }

    void BuyPetReagent(Player* player, Creature* creature, uint32 action)
    {
        uint32 itemEntry = (action == 1) ? ITEM_CORPSE_DUST :
                           (action == 2) ? ITEM_SOUL_SHARD  : 0;
        if (!itemEntry) { CloseGossipMenuFor(player); return; }

        if (!player->HasEnoughMoney(static_cast<uint32>(REAGENT_COST_COPPER)))
        {
            Notify(player, "|cffFF0000[Sanctum]|r Not enough gold.");
            ShowPetShop(player, creature);
            return;
        }
        if (!player->AddItem(itemEntry, REAGENT_BUNDLE_SIZE))
        {
            Notify(player, "|cffFF0000[Sanctum]|r Inventory full.");
            ShowPetShop(player, creature);
            return;
        }
        player->ModifyMoney(-static_cast<int32>(REAGENT_COST_COPPER));
        ShowPetShop(player, creature);
    }

    // --------------------------------------------------------
    // Test helpers — class picker
    // --------------------------------------------------------

    void ShowTestClassPicker(Player* player, Creature* creature, uint32 senderPick)
    {
        static const uint8 classIds[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 11 };
        for (uint8 cid : classIds)
            AddGossipItemFor(player, GOSSIP_ICON_TALK,
                GetClassName(cid).c_str(), senderPick, cid);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", SENDER_TEST_CHAR, 0);
        SendGossipMenuFor(player, NPC_TEXT_WARDEN_WELCOME, creature->GetGUID());
    }

    void AssignTestClass(Player* player, Creature* creature, uint8 slot, uint8 classId)
    {
        if (classId == 0) { HandleTestChar(player, creature, 0); return; }
        uint32 pguid = player->GetGUID().GetCounter();
        if (slot == 2)
        {
            CharacterDatabase.Execute(
                "UPDATE character_multiclass SET class2 = {}, selection_step = GREATEST(selection_step, 1) WHERE guid = {}",
                classId, pguid);
            Notify(player, "|cff00FF00[Test]|r 2nd class set to " + GetClassName(classId) + ".");
        }
        else
        {
            CharacterDatabase.Execute(
                "UPDATE character_multiclass SET class3 = {}, selection_step = GREATEST(selection_step, 2) WHERE guid = {}",
                classId, pguid);
            Notify(player, "|cff00FF00[Test]|r 3rd class set to " + GetClassName(classId) + ".");
        }
        GrantClassSpells(player, classId);
        HandleTestChar(player, creature, 0);
    }

    // --------------------------------------------------------
    // Test character setup — sequential dialogue
    //
    // action=0  intro screen
    // action=1  grant level 60 + class spells  → show bag step
    // action=2  send test bags to mailbox       → show gear step
    // action=3  send Normal tier gear           → re-show gear step
    // action=4  send Enchanted tier gear        → re-show gear step
    // action=5  send Legendary tier gear        → re-show gear step
    // --------------------------------------------------------

    void ShowTestGearMenu(Player* player, Creature* creature)
    {
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
            "[Test] Send Normal tier gear (→ mailbox)",    SENDER_TEST_CHAR, 3);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
            "[Test] Send Enchanted tier gear (→ mailbox)", SENDER_TEST_CHAR, 4);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
            "[Test] Send Epic tier gear (→ mailbox)",      SENDER_TEST_CHAR, 5);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", GOSSIP_SENDER_MAIN, 0);
        SendGossipMenuFor(player, NPC_TEXT_WARDEN_DONE, creature->GetGUID());
    }

    void HandleTestChar(Player* player, Creature* creature, uint32 action)
    {
        switch (action)
        {
            // ── Step 0: intro ──────────────────────────────────────────
            case 0:
            {
                uint32 pguid2 = player->GetGUID().GetCounter();
                WardenData wd = LoadWardenData(pguid2);

                // Class pickers — show current class or "Pick" if unset
                std::string c2label = wd.class2
                    ? ("|cff00FF00[Test] 2nd class: " + GetClassName(wd.class2) + "|r (change)")
                    : "[Test] Pick 2nd class";
                std::string c3label = wd.class3
                    ? ("|cff00FF00[Test] 3rd class: " + GetClassName(wd.class3) + "|r (change)")
                    : "[Test] Pick 3rd class";

                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                    c2label.c_str(), SENDER_TEST_CHAR, 6);
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                    c3label.c_str(), SENDER_TEST_CHAR, 7);
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                    "[Test] Step 1: Grant Level 60 and all class spells",
                    SENDER_TEST_CHAR, 1);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", GOSSIP_SENDER_MAIN, 0);
                SendGossipMenuFor(player, NPC_TEXT_WARDEN_WELCOME, creature->GetGUID());
                return;
            }

            // ── Test: pick 2nd class ───────────────────────────────────
            case 6:
                ShowTestClassPicker(player, creature, SENDER_TEST_CLASS2);
                return;

            // ── Test: pick 3rd class ───────────────────────────────────
            case 7:
                ShowTestClassPicker(player, creature, SENDER_TEST_CLASS3);
                return;

            // ── Step 1: level 60 + class spells ───────────────────────
            case 1:
            {
                if (player->GetLevel() < 60)
                    player->GiveLevel(60);

                player->SetUInt32Value(PLAYER_XP, 0);
                player->SetFullHealth();
                if (player->GetMaxPower(POWER_MANA) > 0)
                    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));

                // Grant spells for all chosen classes
                uint32 pguid = player->GetGUID().GetCounter();
                WardenData wdata = LoadWardenData(pguid);
                if (wdata.class1) GrantClassSpells(player, wdata.class1);
                if (wdata.class2) GrantClassSpells(player, wdata.class2);
                if (wdata.class3) GrantClassSpells(player, wdata.class3);

                player->ModifyMoney(1000 * 10000); // 1000 gold

                Notify(player, "|cff00FF00[Test]|r Level 60 granted. All class spells learned. 1000g added.");

                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                    "[Test] Step 2: Receive 4 large bags (→ mailbox)",
                    SENDER_TEST_CHAR, 2);
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "< Back", GOSSIP_SENDER_MAIN, 0);
                SendGossipMenuFor(player, NPC_TEXT_WARDEN_WELCOME, creature->GetGUID());
                return;
            }

            // ── Step 2: bags ───────────────────────────────────────────
            case 2:
            {
                static const uint32 bags[TEST_BAG_COUNT] = {
                    TEST_BAG_ITEM, TEST_BAG_ITEM, TEST_BAG_ITEM, TEST_BAG_ITEM
                };
                MailItemBatch(player, "Test Bags — Sanctum Warden", bags, TEST_BAG_COUNT);

                Notify(player, "|cff00FF00[Test]|r 4 bags sent to your mailbox. "
                    "Equip them for maximum inventory space before collecting gear.");

                ShowTestGearMenu(player, creature);
                return;
            }

            // ── Step 3: Normal tier gear ───────────────────────────────
            case 3:
            {
                uint32 pguid = player->GetGUID().GetCounter();
                WardenData wd = LoadWardenData(pguid);
                if (!wd.class1 || !wd.class2 || !wd.class3)
                {
                    Notify(player, "|cffFF0000[Test]|r Pick all 3 classes first.");
                    ShowTestGearMenu(player, creature);
                    return;
                }
                std::vector<uint32> base = BuildClassBaseItems(wd.class1, wd.class2, wd.class3);
                MailItemBatch(player, "Test Gear: Normal Tier", base.data(), base.size());
                Notify(player, "|cff00FF00[Test]|r Normal tier class gear sent to your mailbox.");
                ShowTestGearMenu(player, creature);
                return;
            }

            // ── Step 4: Enchanted tier gear ────────────────────────────
            case 4:
            {
                uint32 pguid = player->GetGUID().GetCounter();
                WardenData wd = LoadWardenData(pguid);
                if (!wd.class1 || !wd.class2 || !wd.class3)
                {
                    Notify(player, "|cffFF0000[Test]|r Pick all 3 classes first.");
                    ShowTestGearMenu(player, creature);
                    return;
                }
                std::vector<uint32> base = BuildClassBaseItems(wd.class1, wd.class2, wd.class3);
                std::vector<uint32> toMail;
                for (uint32 id : base)
                {
                    uint32 ench = 0, epic = 0;
                    if (GearTiers_EnsureVariants(id, ench, epic) && ench)
                        toMail.push_back(ench);
                }
                if (!toMail.empty())
                    MailItemBatch(player, "Test Gear: Enchanted Tier", toMail.data(), toMail.size());
                Notify(player, "|cff00FF00[Test]|r Enchanted tier class gear sent to your mailbox.");
                ShowTestGearMenu(player, creature);
                return;
            }

            // ── Step 5: Epic tier gear ─────────────────────────────────
            case 5:
            {
                uint32 pguid = player->GetGUID().GetCounter();
                WardenData wd = LoadWardenData(pguid);
                if (!wd.class1 || !wd.class2 || !wd.class3)
                {
                    Notify(player, "|cffFF0000[Test]|r Pick all 3 classes first.");
                    ShowTestGearMenu(player, creature);
                    return;
                }
                std::vector<uint32> base = BuildClassBaseItems(wd.class1, wd.class2, wd.class3);
                std::vector<uint32> toMail;
                for (uint32 id : base)
                {
                    uint32 ench = 0, epic = 0;
                    if (GearTiers_EnsureVariants(id, ench, epic) && epic)
                        toMail.push_back(epic);
                }
                if (!toMail.empty())
                    MailItemBatch(player, "Test Gear: Epic Tier", toMail.data(), toMail.size());
                Notify(player, "|cff00FF00[Test]|r Epic tier class gear sent to your mailbox.");
                ShowTestGearMenu(player, creature);
                return;
            }

            default:
                CloseGossipMenuFor(player);
                return;
        }
    }
};

// ============================================================
// Class Trainer — entry → class ID lookup
// ============================================================

static uint8 GetTrainerClass(uint32 entry)
{
    switch (entry)
    {
        case TRAINER_WARRIOR:      return WOW_CLASS_WARRIOR;
        case TRAINER_PALADIN:      return WOW_CLASS_PALADIN;
        case TRAINER_HUNTER:       return WOW_CLASS_HUNTER;
        case TRAINER_ROGUE:        return WOW_CLASS_ROGUE;
        case TRAINER_PRIEST:       return WOW_CLASS_PRIEST;
        case TRAINER_DEATH_KNIGHT: return WOW_CLASS_DEATH_KNIGHT;
        case TRAINER_SHAMAN:       return WOW_CLASS_SHAMAN;
        case TRAINER_MAGE:         return WOW_CLASS_MAGE;
        case TRAINER_WARLOCK:      return WOW_CLASS_WARLOCK;
        case TRAINER_DRUID:        return WOW_CLASS_DRUID;
        default:                   return 0;
    }
}

static uint32 GetTrainerNPCTextId(uint8 classId)
{
    switch (classId)
    {
        case WOW_CLASS_WARRIOR:      return NPC_TEXT_TRAINER_WARRIOR;
        case WOW_CLASS_PALADIN:      return NPC_TEXT_TRAINER_PALADIN;
        case WOW_CLASS_HUNTER:       return NPC_TEXT_TRAINER_HUNTER;
        case WOW_CLASS_ROGUE:        return NPC_TEXT_TRAINER_ROGUE;
        case WOW_CLASS_PRIEST:       return NPC_TEXT_TRAINER_PRIEST;
        case WOW_CLASS_DEATH_KNIGHT: return NPC_TEXT_TRAINER_DK;
        case WOW_CLASS_SHAMAN:       return NPC_TEXT_TRAINER_SHAMAN;
        case WOW_CLASS_MAGE:         return NPC_TEXT_TRAINER_MAGE;
        case WOW_CLASS_WARLOCK:      return NPC_TEXT_TRAINER_WARLOCK;
        case WOW_CLASS_DRUID:        return NPC_TEXT_TRAINER_DRUID;
        default:                     return NPC_TEXT_WARDEN_WELCOME;
    }
}

// ============================================================
// Class Trainer Script
// All 10 class trainer NPCs share this script.
// The NPC entry determines which class they represent.
// ============================================================

class SanctumClassTrainerScript : public CreatureScript
{
public:
    SanctumClassTrainerScript() : CreatureScript("npc_sanctum_class_trainer") {}

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        uint8 trainerClass = GetTrainerClass(creature->GetEntry());
        if (!trainerClass) return false;

        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        WardenData data = LoadWardenData(guid);
        ClearGossipMenuFor(player);

        bool isPlayerClass = (data.class1 == trainerClass ||
                              data.class2 == trainerClass ||
                              data.class3 == trainerClass);

        uint8 classCount = (data.class1 ? 1 : 0) +
                           (data.class2 ? 1 : 0) +
                           (data.class3 ? 1 : 0);

        if (isPlayerClass)
        {
            // Already this class — offer training
            std::string label = "Train " + GetClassName(trainerClass) + " abilities.";
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, label.c_str(), SENDER_TRAINER_PAGE, 0);
            SendGossipMenuFor(player, NPC_TEXT_TRAINER_TRAIN, creature->GetGUID());
        }
        else if (classCount < 3)
        {
            // Can still pick a class — show path dialogue
            AddGossipItemFor(player, GOSSIP_ICON_TALK,
                "I wish to walk your path.", GOSSIP_SENDER_MAIN, 1);
            SendGossipMenuFor(player, GetTrainerNPCTextId(trainerClass), creature->GetGUID());
        }
        else
        {
            // Three classes already chosen and this isn't one of them
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "You walk three paths already. No more may be added.", GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, NPC_TEXT_TRAINER_FULL, creature->GetGUID());
        }

        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        ClearGossipMenuFor(player);
        uint8 trainerClass = GetTrainerClass(creature->GetEntry());
        if (!trainerClass) return false;

        uint32 guid = static_cast<uint32>(player->GetGUID().GetCounter());
        WardenData data = LoadWardenData(guid);

        // "I wish to walk your path" — show confirmation
        if (sender == GOSSIP_SENDER_MAIN && action == 1)
        {
            AddGossipItemFor(player, GOSSIP_ICON_TALK,
                "Yes. Bind me to this path.", GOSSIP_SENDER_MAIN, 2);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Not yet. I need more time.", GOSSIP_SENDER_MAIN, 0);
            SendGossipMenuFor(player, NPC_TEXT_TRAINER_CONFIRM, creature->GetGUID());
            return true;
        }

        // Confirmed — assign class
        if (sender == GOSSIP_SENDER_MAIN && action == 2)
        {
            bool isPlayerClass = (data.class1 == trainerClass ||
                                  data.class2 == trainerClass ||
                                  data.class3 == trainerClass);
            uint8 classCount = (data.class1 ? 1 : 0) +
                               (data.class2 ? 1 : 0) +
                               (data.class3 ? 1 : 0);

            if (!isPlayerClass && classCount < 3)
            {
                if (data.class2 == 0)
                {
                    CharacterDatabase.Execute(
                        "UPDATE character_multiclass SET class2 = {}, selection_step = 1 WHERE guid = {}",
                        trainerClass, guid);
                    data.class2 = trainerClass;
                    data.step   = 1;
                    // Credit Quest 700002 objective (bind second class)
                    player->KilledMonsterCredit(CREDIT_SECOND_PATH);
                }
                else if (data.class3 == 0)
                {
                    CharacterDatabase.Execute(
                        "UPDATE character_multiclass SET class3 = {}, selection_step = 2 WHERE guid = {}",
                        trainerClass, guid);
                    data.class3 = trainerClass;
                    data.step   = 2;
                    // Credit Quest 700003 objective (bind third class)
                    player->KilledMonsterCredit(CREDIT_THIRD_PATH);
                    // Send class-specific dungeon set gear for all 3 tiers + 10,000g
                    SendStarterGearPackage(player, data.class1, data.class2, data.class3);
                }

                // Grant all spells available at the player's current level for this class.
                GrantClassSpells(player, trainerClass);

                uint8 newCount = (data.class1 ? 1 : 0) +
                                 (data.class2 ? 1 : 0) +
                                 (data.class3 ? 1 : 0);
                uint8 remaining = 3 - newCount;

                if (remaining > 0)
                    Notify(player, "|cffFF8000[Sanctum]|r Path bound. You may still choose " +
                        std::to_string(remaining) + " more class" +
                        (remaining == 1 ? "" : "es") + ". Seek the other Class Masters.");
                else
                    Notify(player, "|cff00FF00[Sanctum]|r All three paths are bound. "
                        "Speak with the Sanctum Warden to choose your starting zone.");
            }

            CloseGossipMenuFor(player);
            return true;
        }

        // Close button (action 0)
        if (sender == GOSSIP_SENDER_MAIN && action == 0)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        // Show spell training page
        if (sender == SENDER_TRAINER_PAGE)
        {
            ShowSpellPage(player, creature, trainerClass,
                SENDER_TRAINER_LEARN, SENDER_TRAINER_PAGE, action,
                NPC_TEXT_TRAINER_TRAIN);
            return true;
        }

        // Learn a specific spell
        if (sender == SENDER_TRAINER_LEARN)
        {
            uint32 spellId = action;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info || PlayerKnowsCanonical(player, spellId))
            {
                CloseGossipMenuFor(player);
                return true;
            }

            uint32 cost = GetSpellCost(spellId, trainerClass);
            if (cost > 0 && !player->HasEnoughMoney(cost))
            {
                Notify(player, "|cffFF0000[Sanctum]|r Not enough gold to learn that spell.");
                CloseGossipMenuFor(player);
                return true;
            }

            if (cost > 0)
                player->ModifyMoney(-(int32)cost);

            player->learnSpell(spellId, false);
            uint32 taughtId = ResolveTaughtSpell(spellId);
            if (taughtId != spellId && !player->HasSpell(taughtId))
                player->learnSpell(taughtId, false);

            std::string name = (info->SpellName[0]) ? info->SpellName[0] : "Unknown";
            Notify(player, "|cff00FF00[Sanctum]|r Learned: " + name + ".");

            // Reopen page so they can keep training
            ShowSpellPage(player, creature, trainerClass,
                SENDER_TRAINER_LEARN, SENDER_TRAINER_PAGE, 0,
                NPC_TEXT_TRAINER_TRAIN);
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }
};

// ============================================================
// Hunter ammo refill — Ammo is a Disabled System in Sanctum.
// Called on login and level-up to keep the Hunter fully stocked with
// the best ammo their level allows. Detects bow/crossbow vs gun automatically.
// ============================================================

static uint32 GetBestAmmoForLevel(uint8 level, bool useBullets)
{
    // Descending tier list — first entry whose minLevel <= player level wins.
    struct AmmoTier { uint8 minLevel; uint32 arrowId; uint32 bulletId; };
    static const AmmoTier TIERS[] =
    {
        { 80, 52021, 52020 }, // Iceblade Arrow       / Shatter Rounds
        { 75, 41586, 41584 }, // Terrorshaft Arrow    / Frostbite Bullets
        { 72, 41165, 41164 }, // Saronite Razorheads  / Mammoth Cutters
        { 70, 31737, 31735 }, // Timeless Arrow       / Timeless Shell
        { 65, 28056, 28061 }, // Blackflight Arrow    / Ironbite Shell
        { 62, 33803, 23773 }, // Adamantite Stinger   / Adamantite Shells
        { 55, 28053, 28060 }, // Wicked Arrow         / Impact Shot
        { 51, 19316, 19317 }, // Ice Threaded Arrow   / Ice Threaded Bullet
        { 40, 11285, 11284 }, // Jagged Arrow         / Accurate Slugs
        { 25,  3030,  3033 }, // Razor Arrow          / Solid Shot
        { 10,  2515,  2519 }, // Sharp Arrow          / Heavy Shot
        {  1,  2512,  2516 }, // Rough Arrow          / Light Shot
    };
    for (auto const& t : TIERS)
        if (level >= t.minLevel)
            return useBullets ? t.bulletId : t.arrowId;
    return useBullets ? 2516u : 2512u; // fallback
}

static void RefillHunterAmmo(Player* player)
{
    // Only Hunter multiclass players need ammo.
    // Check equipped ranged weapon to decide arrows vs bullets.
    // Default to arrows if no ranged weapon equipped.
    bool useBullets = false;
    Item* ranged = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (ranged && ranged->GetTemplate())
        useBullets = (ranged->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_GUN);

    uint32 ammoId = GetBestAmmoForLevel(player->GetLevel(), useBullets);
    if (!ammoId) return;

    // Top up to a full stack (1000). Remove the previous tier's ammo first if the
    // player has just crossed into a new level bracket so they don't carry two types.
    uint32 current = player->GetItemCount(ammoId);
    static const uint32 AMMO_STACK = 1000;
    if (current >= AMMO_STACK) return;

    uint32 needed = AMMO_STACK - current;
    ItemPosCountVec dest;
    uint8 msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ammoId, needed);
    if (msg == EQUIP_ERR_OK)
        player->StoreNewItem(dest, ammoId, true, Item::GenerateItemRandomPropertyId(ammoId));
}

// ============================================================
// Quest Script — level-10 auto-complete for Quest 700004
// ============================================================

class SanctumQuestScript : public PlayerScript
{
public:
    SanctumQuestScript() : PlayerScript("SanctumQuestScript") {}

    void OnPlayerLogin(Player* player) override
    {
        uint32 guid = player->GetGUID().GetCounter();
        QueryResult result = CharacterDatabase.Query(
            "SELECT class1, class2, class3, selection_step, gear_sent FROM character_multiclass WHERE guid = {}", guid);
        if (!result)
            return;
        Field* f   = result->Fetch();
        uint8 c1   = f[0].Get<uint8>();
        uint8 c2   = f[1].Get<uint8>();
        uint8 c3   = f[2].Get<uint8>();
        uint8 step = f[3].Get<uint8>();
        uint8 sent = f[4].Get<uint8>();
        if (step >= 2 && sent == 0 && c1 && c2 && c3)
            SendStarterGearPackage(player, c1, c2, c3);

        // Ammo is a Disabled System in Sanctum — Hunters should never run out.
        // On every login, give a full stack of the best ammo the player can use at their level.
        // Arrows for bow/crossbow users; bullets for gun users.
        bool isHunter = (c1 == WOW_CLASS_HUNTER || c2 == WOW_CLASS_HUNTER || c3 == WOW_CLASS_HUNTER);
        if (isHunter && player->GetLevel() >= 1)
            RefillHunterAmmo(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        // Refill ammo on level-up so Hunter always has the best usable tier.
        RefillHunterAmmo(player);

        if (player->GetLevel() < 10)
            return;
        if (player->GetQuestStatus(QUEST_WHERE_ADVENTURE) != QUEST_STATUS_INCOMPLETE)
            return;

        // Credit level-10 objective
        player->KilledMonsterCredit(CREDIT_LEVEL_10);

        // If both objectives are now satisfied, auto-reward the quest
        if (player->GetQuestStatus(QUEST_WHERE_ADVENTURE) == QUEST_STATUS_COMPLETE)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(QUEST_WHERE_ADVENTURE);
            if (quest)
            {
                player->RewardQuest(quest, 0, player, false);
                Notify(player, "|cff00FF00[Sanctum]|r Quest completed: Where Adventure Begins. "
                    "Your story has truly begun.");
            }
        }
    }
};

// ============================================================
// Startup prewarm — seed Enchanted/Epic variant DB rows for every base item
// used in class gear packages. After first server restart post-deploy, all
// variants are in the DB and ready. Subsequent restarts load them into ObjectMgr.
// ============================================================

class SanctumGearPrewarmScript : public WorldScript
{
public:
    SanctumGearPrewarmScript() : WorldScript("SanctumGearPrewarmScript") {}

    void OnStartup() override
    {
        uint32 created = 0;

        // All armor sets
        for (int cls = 1; cls <= 11; ++cls)
        {
            if (cls == 10) continue;
            for (int slot = 0; slot < 8; ++slot)
            {
                uint32 id = ARMOR_SET[cls][slot];
                if (!id) continue;
                uint32 ench = 0, epic = 0;
                if (!GearTiers_EnsureVariants(id, ench, epic))
                    ++created;
            }
        }

        // All canonical weapons
        for (uint8 sub = 0; sub < 20; ++sub)
        {
            uint32 id = WEAPON_ITEM_BY_SUBCLASS[sub];
            if (!id) continue;
            uint32 ench = 0, epic = 0;
            if (!GearTiers_EnsureVariants(id, ench, epic))
                ++created;
        }

        // Shield
        {
            uint32 ench = 0, epic = 0;
            if (!GearTiers_EnsureVariants(SHIELD_ITEM, ench, epic))
                ++created;
        }

        // Accessories
        for (size_t i = 0; i < ACCESSORY_COUNT; ++i)
        {
            uint32 ench = 0, epic = 0;
            if (!GearTiers_EnsureVariants(ACCESSORY_ITEMS[i], ench, epic))
                ++created;
        }

        if (created > 0)
            LOG_INFO("module",
                "[mod-sanctum-warden] Pre-warmed {} new gear tier variant(s). "
                "Restart once more for full 3-tier delivery to new characters.",
                created);
        else
            LOG_INFO("module",
                "[mod-sanctum-warden] All class gear tier variants are ready.");
    }
};

// ============================================================
// Registration
// ============================================================

void AddSC_mod_sanctum_warden()
{
    new SanctumWardenScript();
    new SanctumClassTrainerScript();
    new SanctumQuestScript();
    new SanctumGearPrewarmScript();
    LOG_INFO("module", "[mod-sanctum-warden] Module loaded. Warden and Class Masters ready.");
}
