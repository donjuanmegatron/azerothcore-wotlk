// mod-pet-systems.cpp
// Sanctum Pet Systems
//
// Handles three interconnected pet systems:
//
// 1. MULTI-CLASS PET COEXISTENCE
//    WoW 3.3.5a only supports one pet slot (Player::m_pet). To let Warlock,
//    Hunter, DK, Druid, Priest, Shaman, and Mage pets all be out simultaneously,
//    we use a priority + guardian fallback system:
//
//    PRIORITY ORDER for the real pet slot (pet action bar shown in client):
//      Hunter beast          (highest — Hunter keeps real pet slot always; taming requires it)
//      Mage Water Elemental  (second — permanent via Glyph of Eternal Water when no Hunter)
//      Warlock demon         (third — real slot only if no Hunter or Mage)
//
//    All other pet-class pets use GUARDIAN SUMMONS via SummonCreature():
//      - DK ghoul:       always a guardian (Risen Ghoul entry 26125, CastSpell triggered)
//      - Druid treant:   guardian (entry 1964, permanent)
//      - Priest shadow:  guardian (entry 19668, permanent)
//      - Shaman wolves:  guardian (entry 29264, permanent)
//      - Warlock demon:  guardian when Hunter (or Mage) has pet slot
//      - Hunter beast:   guardian only if Mage has slot (rare; Hunter normally wins)
//
//    Guardians follow the player and use REACT_AGGRESSIVE (fight enemies).
//    They don't have a pet action bar but are otherwise fully combat-capable.
//    Guardian GUIDs are tracked per-player so we know when they need re-summoning.
//
// 2. PET ARMORY BAG SYSTEM
//    Custom bag items occupying physical WoW bag slots, one per pet class.
//    Items inside grant stats only to that pet class's companion.
//
// 3. OWNER STAT INHERITANCE
//    Active combat pets (Pet*) receive 40% of owner's primary stats AND 40% of
//    owner's melee attack power on login and level-up.

#include "ScriptMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Creature.h"
#include "Unit.h"
#include "Item.h"
#include "Bag.h"
#include "ItemTemplate.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Log.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "Spell.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "ObjectAccessor.h"
#include "MotionMaster.h"
#include "CommandScript.h"
#include <vector>
#include <unordered_map>
#include <sstream>

using namespace Acore::ChatCommands;

// ============================================================
// Spell IDs — player summon spells
// ============================================================

static const uint32 SPELL_CALL_PET            = 883;
static const uint32 SPELL_REVIVE_PET          = 982;

// ============================================================
// NPC entries for companion creatures
// ============================================================

static const uint32 ENTRY_RISEN_GHOUL         = 26125;  // DK Raise Dead guardian
static const uint32 ENTRY_WATER_ELEMENTAL     = 510;    // Mage permanent Water Elemental
static const uint32 ENTRY_TREANT              = 1964;   // Druid Force of Nature treant
static const uint32 ENTRY_SHADOWFIEND         = 19668;  // Priest Shadowfiend
static const uint32 ENTRY_SPIRIT_WOLF         = 29264;  // Shaman Feral Spirit wolf

// Hunter guardian beast: generic wolf used when real pet slot is taken by higher-priority class.
// The player's actual tamed pet entry varies per character and can't easily be looked up
// without a DB query per tick. This is a stand-in with full visual/stat scaling at player level.
static const uint32 ENTRY_HUNTER_GUARDIAN     = 2707;   // Grey Wolf (placeholder)

// ============================================================
// Warlock demon entries paired with their summon spell IDs
// Used when Warlock must use a guardian instead of the pet slot.
// Priority mirrors EnsureWarlockDemonActive: Felguard first.
// ============================================================

struct WarlockDemonDef { uint32 spell; uint32 entry; };
static const WarlockDemonDef WARLOCK_DEMON_DEFS[] =
{
    { 30146, 17252 }, // Felguard
    {   691,   417 }, // Felhunter
    {   712,  1863 }, // Succubus
    {   697,  1860 }, // Voidwalker
    {   688,   416 }, // Imp
    {     0,     0 }
};

// ============================================================
// Water Elemental pet spell IDs
// ============================================================

static const uint32 SPELL_WATERBOLT           = 31707;  // WE auto-attack (auto-cast)
static const uint32 SPELL_WE_FREEZE           = 33395;  // WE Freeze (manual)

// ============================================================
// Felguard pet spell IDs
// Force-learned at level 10 so the demon has full utility
// regardless of what the pet system grants by default.
//
// Legion Strike — primary damage ability; hits target + nearby.
//   Ranks 1-8 cover levels 1-70. We force rank 1 at level 10
//   and add higher ranks as the player levels. Enabling autocast
//   is the single biggest damage upgrade — without it the Felguard
//   only melees.
// Intercept — charge + 3-sec stun on engage. Manual-cast only.
// Demonic Frenzy — passive aura; stacks attack power during combat.
// ============================================================

static const uint32 SPELL_FELGUARD_LEGION_STRIKE_R1 = 30328;
static const uint32 SPELL_FELGUARD_LEGION_STRIKE_R2 = 30329;
static const uint32 SPELL_FELGUARD_LEGION_STRIKE_R3 = 30330;
static const uint32 SPELL_FELGUARD_LEGION_STRIKE_R4 = 30331;
static const uint32 SPELL_FELGUARD_LEGION_STRIKE_R5 = 30332;
static const uint32 SPELL_FELGUARD_LEGION_STRIKE_R6 = 30333;
static const uint32 SPELL_FELGUARD_LEGION_STRIKE_R7 = 30334;
static const uint32 SPELL_FELGUARD_LEGION_STRIKE_R8 = 30335;
static const uint32 SPELL_FELGUARD_INTERCEPT        = 30153;  // charge + stun
static const uint32 SPELL_FELGUARD_DEMONIC_FRENZY   = 32850;  // passive stacking AP buff

// ============================================================
// Guardian faction — faction 14 (Monster) makes the creature
// hostile to standard world mobs (also faction 14 or aggro-
// reactive). The owner relationship prevents them attacking
// their summoner even though faction 14 is hostile to all.
// This matches how the Water Elemental is configured in SQL.
// ============================================================


// Follow distance/angle for guardian movement (PetDefines.h values)
static const float  GUARDIAN_FOLLOW_DIST      = 1.0f;
static const float  GUARDIAN_FOLLOW_ANGLE     = static_cast<float>(M_PI);

// ============================================================
// Per-player DK summon cooldown (prevents re-cast loop)
// key = player lowguid, value = ms remaining before next attempt
// ============================================================

// ============================================================
// Guardian tracking: player lowguid → (creature entry → creature GUID)
// Checked each WorldScript tick to decide if re-summon is needed.
// ============================================================

static std::unordered_map<uint32, std::unordered_map<uint32, ObjectGuid>> s_guardianGuids;

// ============================================================
// Pet bar spell definitions — abilities shown on the custom bars
// for Warlock (Felguard guardian) and DK (Risen Ghoul guardian).
// These spells are CAST BY THE GUARDIAN (not the player).
//
// Felguard (entry 17252):
//   30328 = Legion Strike R1  — autocast ON  (AoE cleave: hits target + nearby enemies)
//   30153 = Intercept         — autocast OFF (charge + 3s stun; manual — requires 8yd+ range)
//   33698 = Anguish R1        — autocast OFF (Felguard taunt — generates 300 threat, shadow school)
//
// Risen Ghoul (entry 26125):
//   47468 = Claw              — autocast ON  (primary attack)
//   47481 = Gnaw              — autocast OFF (interrupt/stun)
//   47482 = Leap              — autocast OFF (charge to target)
//   47484 = Huddle            — autocast OFF (defensive crouch — reduces incoming damage)
//   2649  = Growl / Taunt     — autocast OFF (toggleable aggro hold)
// ============================================================

struct PetBarSpellDef
{
    uint32 spellId;
    bool   defaultAutocast;
    uint32 fallbackCooldownMs; // used when sSpellMgr has no RecoveryTime
};

static const PetBarSpellDef WARLOCK_BAR_SPELLS[] =
{
    { 30328, true,  6000  }, // Legion Strike R1 (AoE cleave)
    { 30153, false, 30000 }, // Intercept (charge + stun)
    { 33698, false,  5000 }, // Anguish (Felguard taunt)
    { 0,     false, 0     },
};

static const PetBarSpellDef DK_GHOUL_BAR_SPELLS[] =
{
    { 47468, true,  5000  }, // Claw (primary attack)
    { 47481, false, 30000 }, // Gnaw (interrupt/stun)
    { 47482, false, 15000 }, // Leap (charge to target)
    { 47484, false, 45000 }, // Huddle (defensive)
    { 2649,  false, 10000 }, // Growl (taunt)
    { 0,     false, 0     },
};

// Per-player autocast toggle: lowguid → classId → spellId → enabled
static std::unordered_map<uint32, std::unordered_map<uint8, std::unordered_map<uint32, bool>>> s_autocast;

// Per-player spell cooldown remaining (ms): lowguid → classId → spellId → msLeft
static std::unordered_map<uint32, std::unordered_map<uint8, std::unordered_map<uint32, uint32>>> s_spellCd;

// Whether PB_UP has been sent to the client for this guardian: lowguid → classId → bool
static std::unordered_map<uint32, std::unordered_map<uint8, bool>> s_petBroadcastUp;

// Class cache: lowguid → vector of class IDs.
// Populated on login to avoid DB queries on every 1.5s/10s tick.
static std::unordered_map<uint32, std::vector<uint8>> s_classCache;

// Tame suppress: lowguid → remaining 10s ticks to skip guardian re-summon.
// Set by the DIS command so guardians don't re-appear during a 20s Tame Beast channel.
static std::unordered_map<uint32, uint8> s_tameSuppressTicks;

// ============================================================
// Pet AA bonus cache — universal pool shared across all pets.
// Pooled from Beast Mastery (Hunter), Demonology (Warlock),
// and Unholy (DK) pet trees. Populated on login, cleared on logout.
// ============================================================

// Bonus applied to ALL active pets — computed from the player's actual talent ranks.
struct PetAABonus
{
    float  damagePct    = 0.0f;  // % extra damage applied to all pets
    float  inheritExtra = 0.0f;  // additional stat inheritance fraction on top of 40% base
    float  hpPct        = 0.0f;  // % max HP bonus applied to all pets
};

static std::unordered_map<uint32, PetAABonus> s_petAABonus; // key = player lowguid

// Snapshot of flat modifiers last applied to the real Pet* slot.
// Reversed before each re-application to prevent stat stacking across level-ups.
struct AppliedPetStats { float str=0,agi=0,sta=0,intel=0,spi=0,ap=0; };
static std::unordered_map<uint32, AppliedPetStats> s_appliedRealPetStats;

// ---------------------------------------------------------------------------
// Talent-to-pet-bonus table.
// Each entry covers one WoW talent that grants pet bonuses.
// rankSpells[0] = spell learned at rank 1, rankSpells[4] = spell at rank 5 (0 = no such rank).
// The player HasSpell() check on each rank spell finds the current rank efficiently.
// ---------------------------------------------------------------------------
struct TalentBonusEntry
{
    uint32 rankSpells[5];    // per-rank spell IDs (WoW 3.3.5a)
    float  dmgPctPerRank;    // % all-pet damage bonus per rank
    float  inheritPerRank;   // stat-inheritance fraction per rank
    float  hpPctPerRank;     // % all-pet max HP per rank
};

// Curated list of pet-buffing talents from Hunter BM, Warlock Demo, and DK Unholy trees.
// All three trees' bonuses are shared across every active pet (guardian or real pet slot).
static const TalentBonusEntry s_talentBonusTable[] =
{
    // ---- Hunter — Beast Mastery ----
    // Endurance Training (5 ranks): +4% pet max HP per rank
    { {19587, 19588, 19589, 19590, 19591},  0.0f, 0.0f, 4.0f },
    // Unleashed Fury (5 ranks): +4% pet damage per rank
    { {19616, 19617, 19618, 19619, 19620},  4.0f, 0.0f, 0.0f },

    // ---- Warlock — Demonology ----
    // Fel Stamina (3 ranks): +4% demon HP per rank → all pets
    { {18697, 18698, 18699, 0, 0},           0.0f, 0.0f, 4.0f },
    // Unholy Power (5 ranks): +4% demon damage per rank → all pets
    { {18769, 18770, 18771, 18772, 18773},  4.0f, 0.0f, 0.0f },

    // ---- Death Knight — Unholy ----
    // Ravenous Dead (3 ranks): +5% pet damage per rank → all pets
    { {50051, 50052, 50053, 0, 0},          5.0f, 0.0f, 0.0f },
};
static const int s_talentBonusCount = 5;

// Returns the highest rank the player has in a talent (0 if none).
static uint8 GetTalentRank(Player* player, const TalentBonusEntry& entry)
{
    for (int r = 4; r >= 0; --r)
    {
        if (entry.rankSpells[r] == 0) continue;
        if (player->HasSpell(entry.rankSpells[r]))
            return (uint8)(r + 1);
    }
    return 0;
}

// Compute all pet bonuses from the player's current talent investments.
// Called on login and level-up; result cached in s_petAABonus.
static PetAABonus ComputeTalentBonuses(Player* player)
{
    PetAABonus bonus;
    for (int i = 0; i < s_talentBonusCount; ++i)
    {
        uint8 rank = GetTalentRank(player, s_talentBonusTable[i]);
        if (rank == 0) continue;
        bonus.damagePct    += s_talentBonusTable[i].dmgPctPerRank    * rank;
        bonus.inheritExtra += s_talentBonusTable[i].inheritPerRank   * rank;
        bonus.hpPct        += s_talentBonusTable[i].hpPctPerRank     * rank;
    }
    return bonus;
}

// ============================================================
// Pet Armory Bag definitions
// ============================================================

struct PetBagDef
{
    uint32 smallEntry; // pre-60
    uint32 largeEntry; // 60+
    uint8  classId;
};

static const PetBagDef PET_BAG_DEFS[] =
{
    { 700200, 700201,  3 }, // Hunter
    { 700202, 700203,  9 }, // Warlock
    { 700204, 700205,  6 }, // Death Knight
    { 700206, 700207, 11 }, // Druid
    { 700208, 700209,  7 }, // Shaman
    { 700210, 700211,  5 }, // Priest
    { 700212, 700213,  8 }, // Mage
};
static const uint32 PET_BAG_COUNT = 7;

// ============================================================
// Helper: multiclass lookup
// ============================================================

static std::vector<uint8> GetPlayerClasses(Player* player)
{
    uint32 lguid = player->GetGUID().GetCounter();

    auto cacheIt = s_classCache.find(lguid);
    if (cacheIt != s_classCache.end())
        return cacheIt->second;

    // Cache miss — query DB once and store result
    std::vector<uint8> classes;
    QueryResult result = CharacterDatabase.Query(
        "SELECT class1, class2, class3 FROM character_multiclass WHERE guid = {}",
        lguid
    );
    if (!result)
    {
        classes.push_back(player->getClass());
        s_classCache[lguid] = classes;
        return classes;
    }
    Field* fields = result->Fetch();
    for (int i = 0; i < 3; ++i)
    {
        uint8 c = fields[i].Get<uint8>();
        if (c != 0)
            classes.push_back(c);
    }
    s_classCache[lguid] = classes;
    return classes;
}

static bool PlayerHasClass(Player* player, uint8 classId)
{
    for (uint8 c : GetPlayerClasses(player))
        if (c == classId)
            return true;
    return false;
}

// ============================================================
// Helper: pet slot priority
// Only Hunter, Warlock, and Mage compete for the real pet slot.
// DK/Druid/Priest/Shaman always use guardian summons.
// Higher return value = higher priority.
// ============================================================

static uint8 PetSlotPriority(uint8 classId)
{
    switch (classId)
    {
        case CLASS_HUNTER:  return 3;  // Always gets native pet slot — taming requires it
        case CLASS_MAGE:    return 3;  // WE via glyph — ties with Hunter are rare
        case CLASS_WARLOCK: return 2;  // Native slot only when no Hunter present
        default:            return 0;
    }
}

// Returns the class that currently occupies the pet slot, or 0 if empty.
// Identifies class by the pet's creature entry.
static uint8 GetPetSlotClass(Player* player)
{
    Pet* pet = player->GetPet();
    if (!pet)
        return 0;

    uint32 entry = pet->GetEntry();

    if (entry == ENTRY_WATER_ELEMENTAL)
        return CLASS_MAGE;

    for (int i = 0; WARLOCK_DEMON_DEFS[i].spell != 0; ++i)
        if (entry == WARLOCK_DEMON_DEFS[i].entry)
            return CLASS_WARLOCK;

    // Any other pet entry is assumed to be a Hunter beast.
    return CLASS_HUNTER;
}

// ============================================================
// Pet bar helpers — shared between Ensure* functions and PetBars scripts
// ============================================================

static const PetBarSpellDef* GetBarSpells(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARLOCK:      return WARLOCK_BAR_SPELLS;
        case CLASS_DEATH_KNIGHT: return DK_GHOUL_BAR_SPELLS;
        default:                 return nullptr;
    }
}

static uint32 GetGuardianEntryForClass(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARLOCK:      return 17252;  // Felguard
        case CLASS_DEATH_KNIGHT: return 26125;  // Risen Ghoul
        default:                 return 0;
    }
}

// Reverse mapping: creature entry → player class that owns it.
// Used to pick the right pet bag when stats are applied in SummonCombatGuardian.
static uint8 GetClassForGuardianEntry(uint32 entry)
{
    switch (entry)
    {
        case 26125: return CLASS_DEATH_KNIGHT; // Risen Ghoul
        case 1964:  return CLASS_DRUID;        // Treant
        case 19668: return CLASS_PRIEST;       // Shadowfiend
        case 29264: return CLASS_SHAMAN;       // Spirit Wolf
        case 17252: return CLASS_WARLOCK;      // Felguard
        case 2707:  return CLASS_HUNTER;       // Hunter guardian beast
        default:    return 0;
    }
}

// Look up a live guardian for the given class. Checks the GUID map first,
// then falls back to FindNearestCreature for DK ghoul (summoned via CastSpell,
// not SummonCreature, so it may not be in s_guardianGuids).
static Creature* GetGuardianByClass(Player* player, uint8 classId)
{
    uint32 entry = GetGuardianEntryForClass(classId);
    if (!entry) return nullptr;

    uint32 lguid = player->GetGUID().GetCounter();
    auto pIt = s_guardianGuids.find(lguid);
    if (pIt != s_guardianGuids.end())
    {
        auto eIt = pIt->second.find(entry);
        if (eIt != pIt->second.end())
        {
            Creature* c = ObjectAccessor::GetCreature(*player, eIt->second);
            if (c && c->IsAlive()) return c;
        }
    }
    return player->FindNearestCreature(entry, 60.0f, true);
}

// Send a SPBMSG system message intercepted by SanctumPetBars.lua.
// Using SendSysMessage (not PSendSysMessage) avoids printf format-string issues.
// Plain prefix "SPBMSG" avoids WoW client pipe-character processing (|| -> |).
static void SendPB(Player* player, const std::string& payload)
{
    std::string msg = "SPBMSG" + payload;
    ChatHandler(player->GetSession()).SendSysMessage(msg.c_str());
}

// Announce a guardian bar to the client. Sends PB_UP with spell list,
// per-spell autocast state (PB_AC), and initial stance (PB_ST).
static void SendPetBarUp(Player* player, uint8 classId, uint32 entry)
{
    const PetBarSpellDef* defs = GetBarSpells(classId);
    if (!defs) return;

    uint32 lguid = player->GetGUID().GetCounter();
    auto& playerAC = s_autocast[lguid][classId];

    std::string spellList;
    for (int i = 0; defs[i].spellId != 0; ++i)
    {
        if (i > 0) spellList += ",";
        spellList += std::to_string(defs[i].spellId);
        if (playerAC.find(defs[i].spellId) == playerAC.end())
            playerAC[defs[i].spellId] = defs[i].defaultAutocast;
    }

    SendPB(player, "PB_UP:" + std::to_string(classId) + ":" +
                   std::to_string(entry) + ":" + spellList);

    for (int i = 0; defs[i].spellId != 0; ++i)
    {
        bool ac = playerAC[defs[i].spellId];
        SendPB(player, "PB_AC:" + std::to_string(classId) + ":" +
                       std::to_string(defs[i].spellId) + ":" + (ac ? "1" : "0"));
    }

    SendPB(player, "PB_ST:" + std::to_string(classId) + ":A");
    s_petBroadcastUp[lguid][classId] = true;
}

// ============================================================
// Handle a SANCTUM_P:... command from SanctumPetBars.lua.
// body = everything after the "SANCTUM_P:" prefix.
// ============================================================

static void ClearGuardianRecord(Player* player, uint32 entry); // defined below HandlePetBarCommand

static void HandlePetBarCommand(Player* player, const std::string& body)
{
    size_t sep = body.find(':');
    std::string op   = body.substr(0, sep);
    std::string rest = (sep != std::string::npos) ? body.substr(sep + 1) : "";

    // INIT — client just loaded, wants current bar state for all active guardians
    if (op == "INIT")
    {
        static const uint8 BAR_CLASSES[] = { CLASS_WARLOCK, CLASS_DEATH_KNIGHT, 0 };
        for (int i = 0; BAR_CLASSES[i] != 0; ++i)
        {
            uint8 cls = BAR_CLASSES[i];
            if (!PlayerHasClass(player, cls)) continue;
            Creature* g = GetGuardianByClass(player, cls);
            if (g && g->IsAlive())
                SendPetBarUp(player, cls, g->GetEntry());
            else
                SendPB(player, "PB_DOWN:" + std::to_string(cls));
        }
        return;
    }

    // All other commands carry classId as next token
    size_t sep2 = rest.find(':');
    uint8 classId = 0;
    try { classId = (uint8)std::stoul(rest.substr(0, sep2)); } catch (...) { return; }
    std::string args = (sep2 != std::string::npos) ? rest.substr(sep2 + 1) : "";

    if (op == "ATK")
    {
        Creature* g = GetGuardianByClass(player, classId);
        if (!g || !g->IsAlive()) return;
        Unit* target = player->GetSelectedUnit();
        if (!target || !target->IsAlive() || !g->IsValidAttackTarget(target)) return;
        g->SetReactState(REACT_AGGRESSIVE);
        g->GetMotionMaster()->Clear();
        g->AI()->AttackStart(target);
        SendPB(player, "PB_ST:" + std::to_string(classId) + ":A");
    }
    else if (op == "FOL")
    {
        Creature* g = GetGuardianByClass(player, classId);
        if (!g || !g->IsAlive()) return;
        g->SetReactState(REACT_DEFENSIVE);
        g->AttackStop();
        g->GetMotionMaster()->Clear();
        g->GetMotionMaster()->MoveFollow(player, GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
        SendPB(player, "PB_ST:" + std::to_string(classId) + ":D");
    }
    else if (op == "STA")
    {
        Creature* g = GetGuardianByClass(player, classId);
        if (!g || !g->IsAlive()) return;
        g->SetReactState(REACT_PASSIVE);
        g->AttackStop();
        g->GetMotionMaster()->Clear();
        g->GetMotionMaster()->MoveIdle();
        SendPB(player, "PB_ST:" + std::to_string(classId) + ":P");
    }
    else if (op == "AGR")
    {
        Creature* g = GetGuardianByClass(player, classId);
        if (!g || !g->IsAlive()) return;
        g->SetReactState(REACT_AGGRESSIVE);
        g->GetMotionMaster()->Clear();
        g->GetMotionMaster()->MoveFollow(player, GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
        SendPB(player, "PB_ST:" + std::to_string(classId) + ":A");
    }
    else if (op == "CST")
    {
        Creature* g = GetGuardianByClass(player, classId);
        if (!g || !g->IsAlive()) return;
        uint32 spellId = 0;
        try { spellId = std::stoul(args); } catch (...) { return; }
        Unit* target = g->GetVictim() ? g->GetVictim() : player->GetSelectedUnit();
        if (!target || !target->IsAlive()) return;
        g->CastSpell(target, spellId, true);
    }
    else if (op == "DIS")
    {
        // Despawn guardian so taming isn't blocked by "summoned creature" error.
        // Suppress re-summon for 30s (3 × 10s ticks) — enough for a full Tame Beast channel.
        Creature* g = GetGuardianByClass(player, classId);
        if (!g) return;
        uint32 lguid = player->GetGUID().GetCounter();
        ClearGuardianRecord(player, g->GetEntry());
        s_petBroadcastUp[lguid][classId] = false;
        s_tameSuppressTicks[lguid] = 3;
        g->DespawnOrUnsummon();
        SendPB(player, "PB_DOWN:" + std::to_string(classId));
    }
    else if (op == "AUT")
    {
        // AUT:classId:spellId:0|1
        size_t sp = args.find(':');
        if (sp == std::string::npos) return;
        uint32 spellId = 0;
        uint8  enabled = 0;
        try {
            spellId = std::stoul(args.substr(0, sp));
            enabled = (uint8)std::stoul(args.substr(sp + 1));
        } catch (...) { return; }

        uint32 lguid = player->GetGUID().GetCounter();
        s_autocast[lguid][classId][spellId] = (enabled != 0);
        if (enabled) s_spellCd[lguid][classId][spellId] = 0; // fire promptly on re-enable
        SendPB(player, "PB_AC:" + std::to_string(classId) + ":" +
                       std::to_string(spellId) + ":" + std::to_string(enabled));
    }
}

// ============================================================
// Helper: guardian tracking
// ============================================================

static bool IsGuardianAlive(Player* player, uint32 entry)
{
    uint32 lguid = player->GetGUID().GetCounter();
    auto playerIt = s_guardianGuids.find(lguid);
    if (playerIt == s_guardianGuids.end())
        return false;
    auto entryIt = playerIt->second.find(entry);
    if (entryIt == playerIt->second.end())
        return false;

    Creature* creature = ObjectAccessor::GetCreature(*player, entryIt->second);
    return creature && creature->IsAlive();
}

static void RegisterGuardian(Player* player, uint32 entry, Creature* guardian)
{
    s_guardianGuids[player->GetGUID().GetCounter()][entry] = guardian->GetGUID();
}

static void ClearGuardianRecord(Player* player, uint32 entry)
{
    uint32 lguid = player->GetGUID().GetCounter();
    auto it = s_guardianGuids.find(lguid);
    if (it != s_guardianGuids.end())
        it->second.erase(entry);
}

// Remove all guardian records for a player (called on logout/despawn).
static void ClearAllGuardianRecords(Player* player)
{
    s_guardianGuids.erase(player->GetGUID().GetCounter());
}

// ============================================================
// Helper: summon a combat guardian creature
//
// The creature is spawned near the player with:
//   - faction 14 (Monster) so it attacks world mobs
//   - REACT_AGGRESSIVE so it auto-engages nearby enemies
//   - MoveFollow so it trails the player when not in combat
//   - Level set to player level (triggers creature stat recalc)
//
// Returns the spawned Creature* or nullptr on failure.
// ============================================================

// Apply talent-derived pet bonuses to a newly summoned guardian.
static void ApplyGuardianTalentBonus(Player* player, Creature* guardian)
{
    uint32 lguid = player->GetGUID().GetCounter();
    auto it = s_petAABonus.find(lguid);
    if (it == s_petAABonus.end())
        return;

    const PetAABonus& bonus = it->second;

    // % HP bonus from Endurance Training / Fel Stamina
    if (bonus.hpPct > 0.0f)
    {
        uint32 base   = guardian->GetMaxHealth();
        uint32 newMax = base + static_cast<uint32>(base * bonus.hpPct / 100.0f);
        guardian->SetMaxHealth(newMax);
        guardian->SetHealth(newMax);
    }

    // % damage bonus from Unleashed Fury / Unholy Power / Ravenous Dead
    if (bonus.damagePct > 0.0f)
    {
        float mult   = 1.0f + (bonus.damagePct / 100.0f);
        float minDmg = guardian->GetFloatValue(UNIT_FIELD_MINDAMAGE) * mult;
        float maxDmg = guardian->GetFloatValue(UNIT_FIELD_MAXDAMAGE) * mult;
        guardian->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, minDmg);
        guardian->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, maxDmg);
        guardian->UpdateDamagePhysical(BASE_ATTACK);
    }
}

// Scale guardian weapon damage based on player max HP (proxy for level + gear).
// SelectLevel() fixes damage at the template level (80 for Felguard, 1 for Ghoul),
// making them deal level-80 damage regardless of the player's actual level.
// We override base weapon damage here so auto-attacks scale proportionally.
//   Felguard:     3.0-5.0% of player max HP per swing (2.0s attack speed)
//   Risen Ghoul:  1.5-2.5% of player max HP per swing (slightly faster)
// ApplyGuardianTalentBonus() then multiplies these base values by the talent %.
static void ApplyGuardianStatScaling(Player* player, Creature* guardian)
{
    uint32 entry = guardian->GetEntry();
    if (entry != 17252 && entry != ENTRY_RISEN_GHOUL)
        return;

    float playerHP = static_cast<float>(player->GetMaxHealth());
    float minPct, maxPct;

    if (entry == 17252) // Felguard — heavy hitter, 2.0s swing timer
    {
        minPct = 0.030f;
        maxPct = 0.050f;
    }
    else // Risen Ghoul — lighter, slightly faster
    {
        minPct = 0.015f;
        maxPct = 0.025f;
    }

    float minDmg = std::max(playerHP * minPct, 1.0f);
    float maxDmg = std::max(playerHP * maxPct, 2.0f);

    guardian->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, minDmg);
    guardian->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, maxDmg);
    guardian->UpdateDamagePhysical(BASE_ATTACK);
}

static Creature* SummonCombatGuardian(Player* player, uint32 entry)
{
    // Despawn any previously tracked guardian with this entry before spawning a new one.
    // Prevents duplicate creatures when IsGuardianAlive gets a false-negative (stale GUID).
    uint32 lguid = player->GetGUID().GetCounter();
    auto pIt = s_guardianGuids.find(lguid);
    if (pIt != s_guardianGuids.end())
    {
        auto eIt = pIt->second.find(entry);
        if (eIt != pIt->second.end())
        {
            Creature* old = ObjectAccessor::GetCreature(*player, eIt->second);
            if (old)
                old->DespawnOrUnsummon();
            pIt->second.erase(eIt);
        }
    }

    float x, y, z;
    player->GetClosePoint(x, y, z, player->GetObjectSize(), GUARDIAN_FOLLOW_DIST);
    float o = player->GetOrientation();

    Creature* guardian = player->SummonCreature(entry, x, y, z, o, TEMPSUMMON_MANUAL_DESPAWN, 0);
    if (!guardian)
        return nullptr;

    // SetOwnerGUID is required so the creature's AI script can call GetCharmerOrOwner()
    // to find who to follow. Without it, the DK ghoul script finds no owner and stands idle.
    guardian->SetOwnerGUID(player->GetGUID());
    guardian->SetFaction(player->GetFaction());
    guardian->SetLevel(player->GetLevel());

    // SetLevel() updates the level field but does NOT recalculate HP for TempSummons —
    // that only runs at spawn via SelectLevel() using the template level. We override HP
    // directly here so both pets scale with the player's actual max HP (and gear).
    //   Felguard (rare-elite demon): 75% of player HP
    //   Risen Ghoul (normal rank):   50% of player HP
    // Other utility guardians (Treant, Shadowfiend, Wolf) keep their template HP.
    if (entry == 17252 || entry == ENTRY_RISEN_GHOUL)
    {
        float mult  = (entry == 17252) ? 0.75f : 0.50f;
        uint32 newHP = std::max(static_cast<uint32>(player->GetMaxHealth() * mult), 200u);
        guardian->SetCreateHealth(newHP);
        guardian->SetMaxHealth(newHP);
    }

    guardian->SetFullHealth();
    guardian->SetReactState(REACT_AGGRESSIVE);

    // Felguard-specific passive auras — mirror what the real WoW Felguard pet has.
    // Demonic Frenzy: proc aura that stacks 50 AP per melee hit (up to 10 stacks).
    // Avoidance: reduces AoE damage taken by the guardian.
    // Demonic Pact: passive proc buff (on crit, generates spell power bonus).
    if (entry == 17252)
    {
        guardian->AddAura(32850, guardian); // Demonic Frenzy
        guardian->AddAura(32233, guardian); // Avoidance
        guardian->AddAura(48090, guardian); // Demonic Pact
    }

    // Scale base weapon damage to match player level/gear before applying talent %
    ApplyGuardianStatScaling(player, guardian);

    // Apply talent-derived bonuses (HP%, damage%) to this guardian
    ApplyGuardianTalentBonus(player, guardian);

    // Apply 40% owner stat inheritance + pet bag stats immediately on spawn.
    // This runs here so the guardian has correct stats from the moment it appears,
    // rather than waiting for the next login or level-up event.
    uint8 gClass = GetClassForGuardianEntry(entry);
    if (gClass != 0)
        ApplyOwnerAndBagStatsToUnit(player, guardian, gClass);

    // Anchor home position at the player — not at the spawn point.
    // This prevents the evade AI from sending the guardian back to a fixed location
    // as the player moves around. Updated every 3s by the WorldScript tick.
    guardian->SetHomePosition(x, y, z, o);

    guardian->GetMotionMaster()->MoveFollow(player, GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);

    RegisterGuardian(player, entry, guardian);
    return guardian;
}

// ============================================================
// Helper: sum all item stats from a class's pet armory bag.
// Returns zero-filled struct if the bag isn't equipped.
// ============================================================

struct BagStatTotals { float str=0,agi=0,sta=0,intel=0,spi=0; };

static BagStatTotals SumPetBagStats(Player* player, uint8 classId)
{
    BagStatTotals totals;
    bool large = (player->GetLevel() >= 60);
    uint32 targetEntry = 0;
    for (uint32 i = 0; i < PET_BAG_COUNT; ++i)
    {
        if (PET_BAG_DEFS[i].classId == classId)
        {
            targetEntry = large ? PET_BAG_DEFS[i].largeEntry : PET_BAG_DEFS[i].smallEntry;
            break;
        }
    }
    if (!targetEntry)
        return totals;

    Bag* petBag = nullptr;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        Bag* bag = player->GetBagByPos(slot);
        if (bag && bag->GetEntry() == targetEntry) { petBag = bag; break; }
    }
    if (!petBag)
        return totals;

    for (uint32 slot = 0; slot < petBag->GetBagSize(); ++slot)
    {
        Item* item = petBag->GetItemByPos(slot);
        if (!item) continue;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto) continue;
        for (uint32 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        {
            int32 statValue = proto->ItemStat[i].ItemStatValue;
            if (!statValue) continue;
            float val = static_cast<float>(statValue);
            switch (proto->ItemStat[i].ItemStatType)
            {
                case ITEM_MOD_STRENGTH:  totals.str   += val; break;
                case ITEM_MOD_AGILITY:   totals.agi   += val; break;
                case ITEM_MOD_STAMINA:   totals.sta   += val; break;
                case ITEM_MOD_INTELLECT: totals.intel += val; break;
                case ITEM_MOD_SPIRIT:    totals.spi   += val; break;
                default: break;
            }
        }
    }
    return totals;
}

// ============================================================
// Helper: apply 40% owner stat inheritance + pet bag stats
// to any Unit* (real Pet* or guardian Creature*).
//
// Returns a snapshot of what was applied so the caller can
// reverse it before the next call (used for the real pet slot
// which persists across level-ups; guardians are re-created
// fresh each time so they don't need reversal).
// ============================================================

static AppliedPetStats ApplyOwnerAndBagStatsToUnit(Player* player, Unit* target, uint8 classId)
{
    AppliedPetStats applied;

    float inherit = 0.40f;
    uint32 lguid = player->GetGUID().GetCounter();
    auto it = s_petAABonus.find(lguid);
    if (it != s_petAABonus.end())
        inherit += it->second.inheritExtra;

    applied.str   = player->GetStat(STAT_STRENGTH)  * inherit;
    applied.agi   = player->GetStat(STAT_AGILITY)   * inherit;
    applied.sta   = player->GetStat(STAT_STAMINA)   * inherit;
    applied.intel = player->GetStat(STAT_INTELLECT) * inherit;
    applied.spi   = player->GetStat(STAT_SPIRIT)    * inherit;
    applied.ap    = player->GetTotalAttackPowerValue(BASE_ATTACK) * inherit;

    BagStatTotals bag = SumPetBagStats(player, classId);
    applied.str   += bag.str;
    applied.agi   += bag.agi;
    applied.sta   += bag.sta;
    applied.intel += bag.intel;
    applied.spi   += bag.spi;

    target->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  BASE_VALUE, applied.str,   true);
    target->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   BASE_VALUE, applied.agi,   true);
    target->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   BASE_VALUE, applied.sta,   true);
    target->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, BASE_VALUE, applied.intel, true);
    target->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    BASE_VALUE, applied.spi,   true);
    target->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER,   BASE_VALUE, applied.ap,    true);
    target->UpdateAllStats();
    target->UpdateAttackPowerAndDamage();

    return applied;
}

static void ApplyAllPetBonuses(Player* player)
{
    uint32 lguid = player->GetGUID().GetCounter();
    s_petAABonus[lguid] = ComputeTalentBonuses(player);
    const PetAABonus& bonus = s_petAABonus[lguid];

    // ---- Real pet slot ----
    Pet* pet = player->GetPet();
    if (pet && pet->IsAlive())
    {
        // Reverse the previous application before reapplying.
        // The real pet persists across level-ups; without removal the flat modifiers
        // from HandleStatFlatModifier stack permanently with each call.
        auto snapIt = s_appliedRealPetStats.find(lguid);
        if (snapIt != s_appliedRealPetStats.end())
        {
            AppliedPetStats& old = snapIt->second;
            pet->HandleStatFlatModifier(UNIT_MOD_STAT_STRENGTH,  BASE_VALUE, old.str,   false);
            pet->HandleStatFlatModifier(UNIT_MOD_STAT_AGILITY,   BASE_VALUE, old.agi,   false);
            pet->HandleStatFlatModifier(UNIT_MOD_STAT_STAMINA,   BASE_VALUE, old.sta,   false);
            pet->HandleStatFlatModifier(UNIT_MOD_STAT_INTELLECT, BASE_VALUE, old.intel, false);
            pet->HandleStatFlatModifier(UNIT_MOD_STAT_SPIRIT,    BASE_VALUE, old.spi,   false);
            pet->HandleStatFlatModifier(UNIT_MOD_ATTACK_POWER,   BASE_VALUE, old.ap,    false);
        }

        uint8 petClass = GetPetSlotClass(player);
        s_appliedRealPetStats[lguid] = ApplyOwnerAndBagStatsToUnit(player, pet, petClass);

        if (bonus.hpPct > 0.0f)
        {
            uint32 base   = pet->GetMaxHealth();
            uint32 newMax = base + static_cast<uint32>(base * bonus.hpPct / 100.0f);
            pet->SetMaxHealth(newMax);
        }
        if (bonus.damagePct > 0.0f)
        {
            float mult   = 1.0f + (bonus.damagePct / 100.0f);
            float minDmg = pet->GetFloatValue(UNIT_FIELD_MINDAMAGE) * mult;
            float maxDmg = pet->GetFloatValue(UNIT_FIELD_MAXDAMAGE) * mult;
            pet->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, minDmg);
            pet->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, maxDmg);
            pet->UpdateDamagePhysical(BASE_ATTACK);
        }
    }

    // ---- Active guardians ----
    // Guardians are despawned and freshly re-summoned on level-up, so their base stats
    // are always clean — no snapshot reversal needed. Apply owner inheritance + bag stats
    // directly to each one now (covers: DK ghoul, Druid treant, Priest shadowfiend,
    // Shaman wolf, guardian Felguard). SummonCombatGuardian also calls this on spawn so
    // newly summoned guardians get stats immediately without waiting for a level-up event.
    auto gIt = s_guardianGuids.find(lguid);
    if (gIt != s_guardianGuids.end())
    {
        for (auto& [entry, guid] : gIt->second)
        {
            Creature* g = ObjectAccessor::GetCreature(*player, guid);
            if (!g || !g->IsAlive()) continue;
            uint8 classId = GetClassForGuardianEntry(entry);
            if (classId == 0) continue;
            ApplyOwnerAndBagStatsToUnit(player, g, classId);
        }
    }
}

// ============================================================
// Helper: ensure Water Elemental knows Waterbolt + Freeze
// and has Waterbolt on auto-cast. Called from WorldScript tick.
// ============================================================

static void EnsureWaterElementalSpells(Player* player)
{
    Pet* pet = player->GetPet();
    if (!pet || !pet->IsAlive())
        return;
    if (pet->GetEntry() != ENTRY_WATER_ELEMENTAL)
        return;

    bool needsUpdate = false;

    if (!pet->HasSpell(SPELL_WATERBOLT))
    {
        pet->learnSpell(SPELL_WATERBOLT);
        needsUpdate = true;
    }
    if (!pet->HasSpell(SPELL_WE_FREEZE))
    {
        pet->learnSpell(SPELL_WE_FREEZE);
        needsUpdate = true;
    }

    if (needsUpdate)
    {
        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(SPELL_WATERBOLT))
            pet->ToggleAutocast(spellInfo, true);
    }
}

// ============================================================
// Helper: ensure Felguard knows all its abilities and has
// Legion Strike on autocast.
//
// In retail, Felguard is a level 50+ talent demon. In Sanctum,
// players can summon it at level 10. The pet system grants spells
// based on the demon's base level data, which leaves it empty at
// low levels. We force-learn every rank of Legion Strike (the pet
// system only activates the highest valid rank, so granting all is
// safe), Intercept, and Demonic Frenzy.
//
// Only applies to a real Pet* Felguard (pet slot). The guardian
// version (TempSummon) gets its spell behavior via creature_template_spell
// SQL — see 2026_04_20_felguard_spells.sql.
// ============================================================

static void EnsureFelguardSpells(Player* player)
{
    Pet* pet = player->GetPet();
    if (!pet || !pet->IsAlive())
        return;
    if (pet->GetEntry() != 17252) // Felguard
        return;

    bool needsUpdate = false;

    // All Legion Strike ranks — grant every rank so it scales as player levels.
    // The pet action bar will show the highest rank the pet can use.
    static const uint32 legionStrikeRanks[] = {
        SPELL_FELGUARD_LEGION_STRIKE_R1,
        SPELL_FELGUARD_LEGION_STRIKE_R2,
        SPELL_FELGUARD_LEGION_STRIKE_R3,
        SPELL_FELGUARD_LEGION_STRIKE_R4,
        SPELL_FELGUARD_LEGION_STRIKE_R5,
        SPELL_FELGUARD_LEGION_STRIKE_R6,
        SPELL_FELGUARD_LEGION_STRIKE_R7,
        SPELL_FELGUARD_LEGION_STRIKE_R8,
    };
    for (uint32 spellId : legionStrikeRanks)
    {
        if (!pet->HasSpell(spellId))
        {
            pet->learnSpell(spellId);
            needsUpdate = true;
        }
    }

    if (!pet->HasSpell(SPELL_FELGUARD_INTERCEPT))
    {
        pet->learnSpell(SPELL_FELGUARD_INTERCEPT);
        needsUpdate = true;
    }

    if (!pet->HasSpell(SPELL_FELGUARD_DEMONIC_FRENZY))
    {
        pet->learnSpell(SPELL_FELGUARD_DEMONIC_FRENZY);
        needsUpdate = true;
    }

    if (needsUpdate)
    {
        // Enable autocast on the highest Legion Strike rank the pet now has.
        // ToggleAutocast on each rank so whichever one the UI presents is active.
        for (uint32 spellId : legionStrikeRanks)
        {
            if (SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId))
                pet->ToggleAutocast(si, true);
        }
    }
}

// ============================================================
// Helper: re-summon DK ghoul if not present.
// Always a guardian via SummonCreature — never uses the native pet slot.
// No spell cast, no cooldown. Ghoul is a permanent companion that
// auto-respawns via the WorldScript tick when killed.
// ============================================================

static void EnsureDKGhoulActive(Player* player)
{
    if (!PlayerHasClass(player, CLASS_DEATH_KNIGHT))
        return;
    if (player->GetLevel() < 10)
        return;

    if (IsGuardianAlive(player, ENTRY_RISEN_GHOUL))
        return;
    // Re-register if the ghoul exists but wasn't in the GUID map (e.g. after a relog
    // where the old ghoul wasn't despawned). Without this, Mend Pet can't find it.
    if (Creature* existing = player->FindNearestCreature(ENTRY_RISEN_GHOUL, 60.0f, true))
    {
        RegisterGuardian(player, ENTRY_RISEN_GHOUL, existing);
        return;
    }

    SummonCombatGuardian(player, ENTRY_RISEN_GHOUL);
}

// ============================================================
// Helper: re-summon Hunter pet.
// Uses the real pet slot if no higher-priority class has it.
// Falls back to a guardian beast if Warlock or Mage has the slot.
// ============================================================

static void EnsureHunterPetActive(Player* player)
{
    if (!PlayerHasClass(player, CLASS_HUNTER))
        return;
    if (player->GetLevel() < 10)
        return;

    Pet* existingPet = player->GetPet();
    if (existingPet)
    {
        // AzerothCore restores the Warlock's saved demon from character_pet on login
        // before our WorldScript runs. If a Warlock demon is squatting in the slot,
        // evict it so Hunter can claim it on the next tick.
        for (int i = 0; WARLOCK_DEMON_DEFS[i].spell != 0; ++i)
        {
            if (existingPet->GetEntry() == WARLOCK_DEMON_DEFS[i].entry)
            {
                player->RemovePet(existingPet, PET_SAVE_NOT_IN_SLOT);
                return; // Next 10s tick: slot empty, Hunter calls real pet
            }
        }
        return; // Slot already has Hunter's beast (or Mage WE) — nothing to do
    }

    if (player->HasSpell(SPELL_REVIVE_PET))
        player->CastSpell(player, SPELL_REVIVE_PET, true);
    if (player->HasSpell(SPELL_CALL_PET))
        player->CastSpell(player, SPELL_CALL_PET, true);
}

// ============================================================
// Helper: re-summon Warlock demon.
// Uses the real pet slot if no higher-priority class has it.
// Falls back to a guardian demon if Mage has the slot.
// ============================================================

static void EnsureWarlockDemonActive(Player* player)
{
    if (!PlayerHasClass(player, CLASS_WARLOCK))
        return;
    if (player->GetLevel() < 10)
        return;

    // If the player has Hunter or Mage, those classes always own the native pet slot.
    if (PlayerHasClass(player, CLASS_HUNTER) || PlayerHasClass(player, CLASS_MAGE))
    {
        Pet* nativePet = player->GetPet();
        if (nativePet)
        {
            // Only evict it if it's a Warlock demon that auto-restored on login.
            // If it's a Hunter beast or Mage WE, leave it alone — Hunter/Mage owns this slot.
            bool isWarlockDemon = false;
            for (int i = 0; WARLOCK_DEMON_DEFS[i].spell != 0; ++i)
            {
                if (nativePet->GetEntry() == WARLOCK_DEMON_DEFS[i].entry)
                {
                    isWarlockDemon = true;
                    break;
                }
            }
            if (isWarlockDemon)
            {
                player->RemovePet(nativePet, PET_SAVE_AS_CURRENT);
                return; // Let Hunter/Mage claim the slot next tick
            }
            // Hunter beast or Mage WE is legitimately there — fall through to guardian summon
        }
        if (!IsGuardianAlive(player, 17252))
            SummonCombatGuardian(player, 17252);
        return;
    }

    // Warlock is the highest-priority pet class — use native slot.
    Pet* existingPet = player->GetPet();
    if (existingPet)
    {
        if (existingPet->GetEntry() == 17252) // Already Felguard
            return;
        if (player->HasSpell(30146)) // Upgrade current demon to Felguard
        {
            player->RemovePet(existingPet, PET_SAVE_AS_CURRENT);
            return; // Next tick will summon Felguard
        }
        return; // Has a demon, no Felguard spell yet — keep it
    }

    static const uint32 demonSpells[] = { 30146, 691, 712, 697, 688, 0 };
    for (int i = 0; demonSpells[i] != 0; ++i)
    {
        if (player->HasSpell(demonSpells[i]))
        {
            player->CastSpell(player, demonSpells[i], true);
            return;
        }
    }
}

// ============================================================
// Helper: permanent Druid treant guardian.
// Always a guardian — Druid doesn't compete for the pet slot.
// 30% damage reduction applied via SQL (DamageModifier = 0.7).
// ============================================================

static void EnsureDruidTreantActive(Player* player)
{
    if (!PlayerHasClass(player, CLASS_DRUID))
        return;
    if (player->GetLevel() < 10)
        return;

    if (!IsGuardianAlive(player, ENTRY_TREANT))
        SummonCombatGuardian(player, ENTRY_TREANT);
}

// ============================================================
// Helper: permanent Priest shadowfiend guardian.
// Always a guardian — Priest doesn't compete for the pet slot.
// 30% damage reduction applied via SQL (DamageModifier = 0.7).
// ============================================================

static void EnsurePriestShadowfiendActive(Player* player)
{
    if (!PlayerHasClass(player, CLASS_PRIEST))
        return;
    if (player->GetLevel() < 10)
        return;

    if (!IsGuardianAlive(player, ENTRY_SHADOWFIEND))
        SummonCombatGuardian(player, ENTRY_SHADOWFIEND);
}

// ============================================================
// Helper: permanent Shaman spirit wolf guardian.
// Always a guardian — Shaman doesn't compete for the pet slot.
// 30% damage reduction — see SQL note below.
//
// NOTE: Add this to the pet_rework SQL:
//   UPDATE creature_template SET DamageModifier = 0.7 WHERE entry = 29264;
// ============================================================

static void EnsureShamanWolfActive(Player* player)
{
    if (!PlayerHasClass(player, CLASS_SHAMAN))
        return;
    if (player->GetLevel() < 10)
        return;

    if (!IsGuardianAlive(player, ENTRY_SPIRIT_WOLF))
        SummonCombatGuardian(player, ENTRY_SPIRIT_WOLF);
}

// ============================================================
// Player Script
// ============================================================

class PetSystemsPlayerScript : public PlayerScript
{
public:
    PetSystemsPlayerScript() : PlayerScript("PetSystemsPlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player || !player->IsInWorld())
            return;
        GetPlayerClasses(player); // warm class cache — avoids DB queries every tick
        ApplyAllPetBonuses(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!player || !player->IsInWorld())
            return;

        ApplyAllPetBonuses(player);

        // Re-summon all guardians at the new level.
        // Despawn existing guardians so they get re-summoned next tick at player's new level.
        uint32 lguid = player->GetGUID().GetCounter();
        auto it = s_guardianGuids.find(lguid);
        if (it != s_guardianGuids.end())
        {
            for (auto& [entry, guid] : it->second)
            {
                Creature* c = ObjectAccessor::GetCreature(*player, guid);
                if (c && c->IsAlive())
                    c->DespawnOrUnsummon();
            }
            it->second.clear();
        }
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!player || !spell) return;

        uint32 spellId    = spell->GetSpellInfo()->Id;
        // Walk the spell chain to the base rank so that all Mend Pet ranks (136, 3111,
        // 3110, 13543, 13544, 27046, ...) are caught regardless of which rank the player
        // has trained. GetFirstSpellInChain returns the ID unchanged if the spell has no chain.
        uint32 rootSpellId = sSpellMgr->GetFirstSpellInChain(spellId);

        // Mend Pet (root 136) — apply equivalent healing to all active guardians.
        // WoW heals the real pet slot normally; we mirror that to guardian creatures.
        // 20% of max HP = 10 ticks × 2%/tick (standard WotLK Mend Pet output).
        static const uint32 SPELL_MEND_PET_ROOT = 136;
        // Bestial Wrath (19574) — apply the same enrage aura to all active guardians.
        // The aura grants +50% damage and CC immunity for 18 seconds.
        static const uint32 SPELL_BESTIAL_WRATH = 19574;

        bool isMendPet      = (rootSpellId == SPELL_MEND_PET_ROOT);
        bool isBestialWrath = (spellId == SPELL_BESTIAL_WRATH);

        if (!isMendPet && !isBestialWrath)
            return;

        uint32 lguid = player->GetGUID().GetCounter();
        auto it = s_guardianGuids.find(lguid);
        if (it == s_guardianGuids.end() || it->second.empty()) return;

        for (auto& [entry, guid] : it->second)
        {
            Creature* g = ObjectAccessor::GetCreature(*player, guid);
            if (!g || !g->IsAlive()) continue;

            if (isMendPet)
            {
                // Apply the Mend Pet HoT aura with the player as caster. This produces
                // the green heal visual on the guardian AND lets AzerothCore tick the
                // periodic heal normally. SetHealth alone sends no aura packet and shows
                // no particles. Use spellId (not rootSpellId) so higher ranks heal more.
                player->AddAura(spellId, g);
            }
            else if (isBestialWrath)
            {
                // Apply the Bestial Wrath enrage aura directly to the guardian.
                // The aura handles its own 18-second duration and expiry cleanup.
                g->AddAura(SPELL_BESTIAL_WRATH, g);
            }
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        uint32 lguid = player->GetGUID().GetCounter();
        // Despawn guardians before clearing the map so they don't persist as unregistered
        // orphans on the next login. Without this, EnsureDKGhoulActive finds them via
        // FindNearestCreature and returns without re-registering, breaking Mend Pet.
        auto it = s_guardianGuids.find(lguid);
        if (it != s_guardianGuids.end())
        {
            for (auto& [entry, guid] : it->second)
            {
                Creature* g = ObjectAccessor::GetCreature(*player, guid);
                if (g) g->DespawnOrUnsummon();
            }
        }
        ClearAllGuardianRecords(player);
        s_classCache.erase(lguid);
        s_tameSuppressTicks.erase(lguid);
        s_petAABonus.erase(lguid);
        s_appliedRealPetStats.erase(lguid);
    }
};

// ============================================================
// World Script — 10-second periodic tick
//
// Per-player actions (all gated at level >= 10):
//   DK      — re-summon risen ghoul guardian if not present (30s cooldown guard)
//   Hunter  — real pet slot or guardian beast if slot taken by higher-priority class
//   Warlock — real pet slot or guardian demon if Mage has slot
//   Druid   — permanent treant guardian
//   Priest  — permanent shadowfiend guardian
//   Shaman  — permanent spirit wolf guardian
//   Mage    — ensure water elemental knows Waterbolt + Freeze with auto-cast
// ============================================================

class PetSystemsWorldScript : public WorldScript
{
public:
    PetSystemsWorldScript() : WorldScript("PetSystemsWorldScript") {}

    void OnUpdate(uint32 diff) override
    {
        _summonTimer += diff;
        _followTimer += diff;

        bool doSummon = (_summonTimer >= 10000);
        bool doFollow = (_followTimer >= 3000);

        if (!doSummon && !doFollow)
            return;

        if (doSummon) _summonTimer = 0;
        if (doFollow) _followTimer = 0;

        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            if (!session) continue;
            Player* player = session->GetPlayer();
            if (!player || !player->IsInWorld()) continue;
            if (player->GetLevel() < 10) continue;

            // While dead: despawn all guardians so they don't block the spirit healer,
            // then skip all pet logic until the player is alive again.
            if (!player->IsAlive())
            {
                if (doSummon)
                {
                    uint32 lguid = player->GetGUID().GetCounter();
                    auto it = s_guardianGuids.find(lguid);
                    if (it != s_guardianGuids.end() && !it->second.empty())
                    {
                        for (auto& [entry, guid] : it->second)
                        {
                            Creature* g = ObjectAccessor::GetCreature(*player, guid);
                            if (g) g->DespawnOrUnsummon();
                        }
                        it->second.clear();
                    }
                }
                continue;
            }

            // ---- Guardian follow maintenance (every 3s) ----
            // Keep home position anchored to the player so the evade AI never sends
            // guardians back to a fixed spawn point. Re-apply MoveFollow whenever a
            // guardian finishes combat and is just standing idle.
            if (doFollow)
            {
                uint32 lguid = player->GetGUID().GetCounter();
                auto it = s_guardianGuids.find(lguid);
                if (it != s_guardianGuids.end())
                {
                    for (auto& [entry, guid] : it->second)
                    {
                        Creature* g = ObjectAccessor::GetCreature(*player, guid);
                        if (!g || !g->IsAlive()) continue;

                        // Always update home so evade returns near the player, not old spawn
                        g->SetHomePosition(player->GetPositionX(), player->GetPositionY(),
                                           player->GetPositionZ(), player->GetOrientation());

                        // Re-apply follow when not actively fighting
                        if (!g->IsInCombat())
                        {
                            g->GetMotionMaster()->Clear();
                            g->GetMotionMaster()->MoveFollow(player,
                                GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
                        }
                    }
                }
            }

            // ---- Summon checks (every 10s) — order matters ----
            if (doSummon)
            {
                uint32 lguid = player->GetGUID().GetCounter();

                // Tick down tame suppress. While active, skip guardian re-summons so
                // the 20s Tame Beast channel isn't interrupted by a guardian reappearing.
                auto supIt = s_tameSuppressTicks.find(lguid);
                bool tameActive = (supIt != s_tameSuppressTicks.end() && supIt->second > 0);
                if (tameActive)
                    supIt->second--;

                if (PlayerHasClass(player, CLASS_MAGE))
                    EnsureWaterElementalSpells(player);

                // Hunter must run before Warlock to claim native slot first
                if (PlayerHasClass(player, CLASS_HUNTER))
                    EnsureHunterPetActive(player);

                if (!tameActive)
                {
                    if (PlayerHasClass(player, CLASS_WARLOCK))
                    {
                        EnsureWarlockDemonActive(player);
                        EnsureFelguardSpells(player);
                    }

                    if (PlayerHasClass(player, CLASS_DEATH_KNIGHT))
                        EnsureDKGhoulActive(player);

                    if (PlayerHasClass(player, CLASS_DRUID))
                        EnsureDruidTreantActive(player);

                    if (PlayerHasClass(player, CLASS_PRIEST))
                        EnsurePriestShadowfiendActive(player);

                    if (PlayerHasClass(player, CLASS_SHAMAN))
                        EnsureShamanWolfActive(player);
                }
            }
        }
    }

private:
    uint32 _summonTimer = 0;
    uint32 _followTimer = 0;
};

// ============================================================
// Pet Bar Player Script
// Intercepts SANCTUM_P addon messages from SanctumPetBars.lua
// and routes them to HandlePetBarCommand.
// ============================================================

class PetBarsPlayerScript : public PlayerScript
{
public:
    PetBarsPlayerScript() : PlayerScript("PetBarsPlayerScript") {}

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang,
                                       std::string& msg) override
    {
        if ((Language)lang != LANG_ADDON)
            return;
        if (type != CHAT_MSG_PARTY && type != CHAT_MSG_WHISPER)
            return;

        static const std::string PREFIX = "SANCTUM_P:";
        size_t pos = msg.find(PREFIX);
        if (pos == std::string::npos)
            return;

        HandlePetBarCommand(player, msg.substr(pos + PREFIX.size()));
    }

    void OnPlayerLogout(Player* player) override
    {
        uint32 lguid = player->GetGUID().GetCounter();
        s_autocast.erase(lguid);
        s_spellCd.erase(lguid);
        s_petBroadcastUp.erase(lguid);
    }
};

// ============================================================
// Pet Bar World Script — 1.5-second tick
//
// For each online player who has Warlock or DK:
//   • Detect guardian alive/dead transitions → send PB_UP or PB_DOWN
//   • Broadcast HP changes via PB_HP (only on delta)
//   • Run autocast scheduler: cast enabled spells when off cooldown and
//     guardian has a living victim
// ============================================================

class PetBarsWorldScript : public WorldScript
{
public:
    PetBarsWorldScript() : WorldScript("PetBarsWorldScript") {}

    void OnUpdate(uint32 diff) override
    {
        _timer += diff;
        if (_timer < 1500)
            return;
        _timer = 0;

        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            if (!session) continue;
            Player* player = session->GetPlayer();
            if (!player || !player->IsInWorld()) continue;

            uint32 lguid = player->GetGUID().GetCounter();

            static const uint8 BAR_CLASSES[] = { CLASS_WARLOCK, CLASS_DEATH_KNIGHT, 0 };
            for (int ci = 0; BAR_CLASSES[ci] != 0; ++ci)
            {
                uint8 classId = BAR_CLASSES[ci];
                if (!PlayerHasClass(player, classId)) continue;

                Creature* g    = GetGuardianByClass(player, classId);
                bool alive     = g && g->IsAlive();
                bool wasUp     = s_petBroadcastUp[lguid][classId];

                // Transition: guardian appeared
                if (alive && !wasUp)
                {
                    SendPetBarUp(player, classId, g->GetEntry());
                }
                // Transition: guardian died or despawned
                else if (!alive && wasUp)
                {
                    SendPB(player, "PB_DOWN:" + std::to_string(classId));
                    s_petBroadcastUp[lguid][classId] = false;
                }

                if (!alive)
                    continue;

                // HP broadcast — only send when value changes
                uint32 maxHp = g->GetMaxHealth();
                uint32 hpPct = maxHp > 0 ? (g->GetHealth() * 100) / maxHp : 0;
                uint32& lastHp = _lastHp[lguid][classId];
                if (hpPct != lastHp)
                {
                    SendPB(player, "PB_HP:" + std::to_string(classId) + ":" +
                                   std::to_string(hpPct));
                    lastHp = hpPct;
                }

                // Cooldown tick
                auto& playerCd = s_spellCd[lguid][classId];
                for (auto& [sid, cd] : playerCd)
                    if (cd > 0)
                        cd = (cd > 1500) ? cd - 1500 : 0;

                // Autocast scheduler
                Unit* victim = g->GetVictim();
                if (!victim || !victim->IsAlive())
                    continue;

                const PetBarSpellDef* defs = GetBarSpells(classId);
                if (!defs) continue;

                auto& playerAC = s_autocast[lguid][classId];

                for (int i = 0; defs[i].spellId != 0; ++i)
                {
                    uint32 spellId = defs[i].spellId;

                    auto acIt = playerAC.find(spellId);
                    if (acIt == playerAC.end() || !acIt->second)
                        continue; // autocast off

                    auto cdIt = playerCd.find(spellId);
                    if (cdIt != playerCd.end() && cdIt->second > 0)
                        continue; // still on cooldown

                    // Charge spells (Intercept 30153, Leap 47482) only make sense as autocast
                    // when the guardian is at range — in melee the charge fires with no movement,
                    // making it invisible and surprising. Wait until target is 8+ yards away.
                    if (spellId == 30153 || spellId == 47482)
                    {
                        if (g->GetDistance(victim) < 8.0f)
                            continue;
                    }

                    g->CastSpell(victim, spellId, true);

                    SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId);
                    uint32 cooldown = si ? si->RecoveryTime : 0;
                    if (cooldown == 0)
                        cooldown = defs[i].fallbackCooldownMs;
                    playerCd[spellId] = cooldown;
                }
            }
        }
    }

private:
    uint32 _timer = 0;
    // Last HP % sent per guardian — avoids flooding the client each tick
    std::unordered_map<uint32, std::unordered_map<uint8, uint32>> _lastHp;
};

// ============================================================
// .petbonus command — shows talent-derived pet bonuses for self/target
// ============================================================

class mod_pet_bonus_commandscript : public CommandScript
{
public:
    mod_pet_bonus_commandscript() : CommandScript("mod_pet_bonus_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "petbonus", HandlePetBonusCommand, SEC_PLAYER, Console::No },
        };
        return table;
    }

    static bool HandlePetBonusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player) return false;

        // Force a fresh calculation
        PetAABonus bonus = ComputeTalentBonuses(player);
        s_petAABonus[player->GetGUID().GetCounter()] = bonus;

        handler->PSendSysMessage("|cff00ccff[PetBonus]|r Talent-derived pet bonuses:");
        handler->PSendSysMessage("  Damage bonus: {} pct", (int)bonus.damagePct);
        handler->PSendSysMessage("  HP bonus:     {} pct", (int)bonus.hpPct);
        handler->PSendSysMessage("  Stat inherit: {} pct total (40 base + {} talent)",
            (int)((0.40f + bonus.inheritExtra) * 100.0f),
            (int)(bonus.inheritExtra * 100.0f));

        handler->PSendSysMessage("|cff00ccff[PetBonus]|r Talent breakdown:");
        static const char* TALENT_NAMES[] =
        {
            "Endurance Training (Hunter BM)",
            "Unleashed Fury (Hunter BM)",
            "Fel Stamina (Warlock Demo)",
            "Unholy Power (Warlock Demo)",
            "Ravenous Dead (DK Unholy)",
        };
        bool anyTalent = false;
        for (int i = 0; i < s_talentBonusCount; ++i)
        {
            uint8 rank = GetTalentRank(player, s_talentBonusTable[i]);
            if (rank > 0)
            {
                handler->PSendSysMessage("  {}  Rank {}", TALENT_NAMES[i], (uint32)rank);
                anyTalent = true;
            }
        }
        if (!anyTalent)
            handler->SendSysMessage("  No pet talents detected");

        handler->PSendSysMessage("|cff00ccff[PetBonus]|r Active guardian HP:");
        uint32 lguid = player->GetGUID().GetCounter();
        auto gIt = s_guardianGuids.find(lguid);
        bool anyGuardian = false;
        if (gIt != s_guardianGuids.end())
        {
            for (auto& [entry, guid] : gIt->second)
            {
                Creature* g = ObjectAccessor::GetCreature(*player, guid);
                if (g && g->IsAlive())
                {
                    handler->PSendSysMessage("  Entry {}  HP: {} / {}", entry, g->GetHealth(), g->GetMaxHealth());
                    anyGuardian = true;
                }
            }
        }
        if (!anyGuardian)
            handler->SendSysMessage("  No active guardians found");

        Pet* pet = player->GetPet();
        if (pet && pet->IsAlive())
            handler->PSendSysMessage("  Real pet HP: {} / {}", pet->GetHealth(), pet->GetMaxHealth());

        return true;
    }
};

// ============================================================
// Soul Link — damage sharing between player and Felguard guardian
//
// Native Soul Link (spell 19028 / 25228) targets the real Pet* demon slot.
// When the Felguard is a guardian TempSummon (player has Hunter or Mage),
// the real pet slot is occupied by another class and Soul Link can't target
// the guardian. This UnitScript implements the same 20% damage split manually:
//   • Player takes damage AND has Felguard guardian alive → 20% to guardian
//
// Guard: if the native Soul Link aura IS active (Felguard in real pet slot),
// HasAura() returns true and we skip to avoid double-dipping. The native
// SPLIT_DAMAGE_PCT mechanic already handles that case.
// ============================================================

static const uint32 SPELL_SOUL_LINK_R1   = 19028;
static const uint32 SPELL_SOUL_LINK_R2   = 25228;
static bool         s_soulLinkRedirecting = false;

class SoulLinkUnitScript : public UnitScript
{
public:
    SoulLinkUnitScript() : UnitScript("SoulLinkUnitScript") {}

    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (s_soulLinkRedirecting) return;
        if (!victim || damage == 0)  return;
        if (victim->GetTypeId() != TYPEID_PLAYER) return;

        Player* player = victim->ToPlayer();

        // Only Warlocks with the Soul Link talent
        if (!PlayerHasClass(player, CLASS_WARLOCK)) return;
        bool hasTalent = player->HasSpell(SPELL_SOUL_LINK_R1) || player->HasSpell(SPELL_SOUL_LINK_R2);
        if (!hasTalent) return;

        // Native Soul Link aura is already active (Felguard in real pet slot) — skip
        if (player->HasAura(SPELL_SOUL_LINK_R1) || player->HasAura(SPELL_SOUL_LINK_R2)) return;

        // Guardian Felguard must be alive
        Creature* felguard = GetGuardianByClass(player, CLASS_WARLOCK);
        if (!felguard || !felguard->IsAlive()) return;

        // 20% of incoming player damage is redirected to the Felguard guardian
        uint32 redirect = damage / 5;
        if (redirect == 0) return;

        damage -= redirect;

        s_soulLinkRedirecting = true;
        Unit::DealDamage(felguard, felguard, redirect, nullptr, DIRECT_DAMAGE,
                         SPELL_SCHOOL_MASK_SHADOW, nullptr, false);
        s_soulLinkRedirecting = false;
    }
};

// ============================================================
// Registration
// ============================================================

void AddSC_mod_pet_systems()
{
    new PetSystemsPlayerScript();
    new PetSystemsWorldScript();
    new PetBarsPlayerScript();
    new PetBarsWorldScript();
    new mod_pet_bonus_commandscript();
    new SoulLinkUnitScript();
    LOG_INFO("module", "[mod-pet-systems] Module loaded. "
             "Pet priority: Hunter=native slot, Warlock/DK=guardian. "
             "SanctumPetBars active for Warlock + DK guardian ability bars.");
}
