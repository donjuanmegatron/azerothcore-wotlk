// aa_runtime.h
//
// Sanctum AA System — shared enum and runtime query API.
// Include this in any file that needs to check a player's AA rank
// (combat modifiers, procs, active spell scripts, etc.).
//
// Usage:
//   #include "aa_runtime.h"
//   uint8 rank = SanctumAA::GetRank(player, AA_G_DOUBLE_STRIKE);
//   if (SanctumAA::Has(player, AA_G_VITAL_HUNGER)) { ... }

#pragma once
#include "Define.h"

class Player;

// ---------------------------------------------------------------------------
// Master ID enum — all approved AAs.
// Source: sanctum_aa_master_list.txt v2.0 (2026-05-09)
//
// ID ranges:
//   2000-2099  General — Offensive
//   2100-2199  General — Defensive
//   2200-2299  General — Utility
//   3000-3099  Pet — Offense
//   3100-3199  Pet — Defense
//   3200-3299  Pet — Utility
//   4100-4199  Archetype — Tank
//   4200-4299  Archetype — DPS
//   4300-4399  Archetype — Healer
//   5000-5099  Class — Warrior
//   5100-5199  Class — Paladin
//   5200-5299  Class — Hunter
//   5300-5399  Class — Rogue
//   5400-5499  Class — Priest
//   5500-5599  Class — Death Knight
//   5600-5699  Class — Shaman
//   5700-5799  Class — Mage
//   5800-5899  Class — Warlock
//   5900-5999  Class — Druid
//   9001       Temper (starter — free at creation)
// ---------------------------------------------------------------------------

enum SanctumAAId : uint32
{
    // ── GENERAL — OFFENSIVE (2001–2019) ────────────────────
    AA_G_DOUBLE_STRIKE    = 2001,   // 3%/6%/10% chance to auto-hit twice for 50% damage
    AA_G_PRECISION        = 2002,   // +5%/+10%/+15% crit damage (melee, ranged, spell)
    AA_G_CRITICAL_MASS    = 2003,   // +1%/+2%/+3% crit chance (all)
    AA_G_KILLING_BLOW     = 2004,   // +10%/+20%/+30% damage to targets below 20% HP
    AA_G_VENGEANCE        = 2005,   // After burst damage taken, +10%/+20%/+30% dmg for 8s
    AA_G_ATTENTION        = 2006,   // +2%/+4%/+6% damage per enemy with you as top threat (cap 5)
    AA_G_INSPIRATION      = 2007,   // Resource gen gives +1% damage stacks (max 5)
    AA_G_SAVAGERY         = 2008,   // Convert dodge rating to flat damage
    AA_G_FEROCITY         = 2009,   // Convert parry rating to flat damage
    AA_G_THOUSAND_CUTS    = 2010,   // Hits apply stacking +1%/+2%/+3% damage taken debuff on enemy
    AA_G_SCHOOL_FIRE      = 2011,   // +3%/+7%/+12% fire damage done
    AA_G_SCHOOL_FROST     = 2012,   // +3%/+7%/+12% frost damage done
    AA_G_SCHOOL_SHADOW    = 2013,   // +3%/+7%/+12% shadow damage done
    AA_G_SCHOOL_HOLY      = 2014,   // +3%/+7%/+12% holy damage done
    AA_G_SCHOOL_NATURE    = 2015,   // +3%/+7%/+12% nature damage done
    AA_G_SCHOOL_ARCANE    = 2016,   // +3%/+7%/+12% arcane damage done
    AA_G_SCHOOL_PHYSICAL  = 2017,   // +3%/+7%/+12% physical damage done
    AA_G_OUTBURST         = 2018,   // +5%/+10%/+15% AoE damage done
    AA_G_BERSERKERS_EDGE  = 2019,   // ONE-SHOT: +20% damage below 30% HP

    // ── GENERAL — DEFENSIVE (2101–2122) ────────────────────
    AA_G_IRON_WILL        = 2101,   // +1%/+2%/+3% max HP
    AA_G_THICK_HIDE       = 2102,   // +1%/+2%/+3% physical damage reduction
    AA_G_WARDING          = 2103,   // +1%/+2%/+3% magical damage reduction
    AA_G_NATURAL_RENEWAL  = 2104,   // +25/+60/+110 HP per 5s
    AA_G_REANIMATION      = 2105,   // Below 20% HP regen 2%/4%/6% missing HP/sec
    AA_G_VITALITY         = 2106,   // +5%/+10%/+15% healing received
    AA_G_HARDENING        = 2107,   // Hits grant +1% DR stacks (rank increases cap)
    AA_G_BULWARK          = 2108,   // Spike hit cap at 35%/30%/25% max HP
    AA_G_HINDSIGHT        = 2109,   // After spike hit absorb shield 30%/60%/100% of that hit
    AA_G_FREE_WILL        = 2110,   // ONE-SHOT: first CC each combat auto-removed
    AA_G_RECOVERY         = 2111,   // 5%/10%/15% of damage taken healed back over 6s
    AA_G_COMBAT_AGILITY   = 2121,   // +2%/+4%/+7% dodge/parry/block + defense rating
    AA_G_CHANNELING_FOCUS = 2122,   // 25%/50%/75% less spell pushback

    // ── GENERAL — UTILITY (2201–2209) ──────────────────────
    AA_G_SPRINTER         = 2201,   // +5%/+10%/+15% out-of-combat move speed
    AA_G_MANA_SURGE       = 2202,   // +2%/+4%/+6% mana regeneration
    AA_G_VITAL_HUNGER     = 2203,   // +5%/+10%/+15% lifesteal (stacks on 5% baseline)
    AA_G_ENDURING_RITES   = 2204,   // ONE-SHOT: self-buffs with base duration >= 1 min become permanent
    AA_G_ZEAL             = 2205,   // At 0 resource, generate 10%/20%/30% of max. Once per full cycle
    AA_G_TEMPERED_BODY    = 2206,   // -10%/-20%/-30% durability loss on death
    AA_G_LUCKY_FIND       = 2207,   // +5%/+10%/+15% chance for double GXP or Tier Shard on kills
    AA_G_CAUTION          = 2208,   // -X% enemy aggro radius
    AA_G_SANCTUM_ESSENCE  = 2209,   // +20/+20/+20 to all primary stats per rank (total +60 at R3)

    // ── PET — OFFENSE (3001–3005) ──────────────────────────
    AA_P_COMMAND          = 3001,   // +5%/+10%/+15% pet damage done
    AA_P_MASTERS_BOND     = 3002,   // +12%/+25%/+40% pet damage done
    AA_P_PACK_TACTICS     = 3003,   // +3%/+6%/+10% pet crit chance
    AA_P_PREDATORS_HOWL   = 3004,   // Pet attacks have chance to apply +3% damage taken debuff
    AA_P_SAVAGE_FLURRY    = 3005,   // Pet auto hits have chance to strike a third time for 50% dmg
    AA_P_BLOODSCENT       = 3006,   // +10/20/30% pet damage to targets below 35% HP

    // ── PET — DEFENSE (3101–3106) ──────────────────────────
    AA_P_HARDENED_HIDE     = 3101,  // +10%/+20%/+30% pet armor
    AA_P_IRON_CONSTITUTION = 3102,  // +5%/+10%/+15% pet max HP
    AA_P_HANDLER           = 3103,  // -5%/-10%/-15% pet damage taken
    AA_P_UNCRUSHABLE       = 3104,  // ONE-SHOT: pet cannot be crit or receive crushing blows
    AA_P_STEELED_RESOLVE   = 3105,  // +2%/+4%/+7% pet dodge/parry/block + defense rating
    AA_P_GUARDIANS_RESOLVE = 3106,  // While pet holds threat: -3%/-6%/-10% dmg from that enemy
    AA_P_MENDING_BOND      = 3107,  // Pet/guardian regens 2/4/6% max HP/s while below 50% HP

    // ── PET — UTILITY (3201–3205) ──────────────────────────
    AA_P_ASSIST_ME         = 3201,  // ONE-SHOT: all pets attack your target. No CD.
    AA_P_REDIRECTION       = 3202,  // ONE-SHOT: pet intercepts one spell aimed at you. 60s ICD.
    AA_P_PACK_LEADER       = 3203,  // Pet attacks heal you for 2%/4%/6% of damage dealt
    AA_P_SOUL_BOND         = 3204,  // ONE-SHOT: on pet death gain 5s fear immunity then re-summon
    AA_P_STAT_INHERITANCE  = 3205,  // Pet inherits 50%/60%/70% of owner primary stats (from 40%)

    // ── ARCHETYPE — TANK (4101–4106) ───────────────────────
    AA_T_STALWART        = 4101,    // +3%/+6%/+10% block value
    AA_T_IRON_RESOLVE    = 4102,    // While holding top threat: -3%/-6%/-10% dmg from that enemy
    AA_T_ANCHORED        = 4103,    // Stun/knockback duration -25%/-50%/-75%
    AA_T_LAST_STAND      = 4104,    // ONE-SHOT: below 25% HP gain 20% DR for 8s. 90s ICD.
    AA_T_TITANS_BLOOD    = 4105,    // +2%/+3%/+3% max HP (req Warrior/Paladin/DK as a class)
    AA_T_DOUBLE_RIPOSTE  = 4106,    // After dodge/parry, auto counter for 25%/40%/60% weapon dmg

    // ── ARCHETYPE — DPS (4201–4206) ────────────────────────
    AA_D_BLOODLETTING    = 4201,    // Crits cause bleed for 20%/35%/50% of crit damage over 8s
    AA_D_HASTE_SURGE     = 4202,    // +5%/+10%/+15% haste beyond 100% hard cap
    AA_D_MORTAL_STRIKE   = 4203,    // Your damage applies 30% healing reduction for 6s
    AA_D_APEX_PREDATOR   = 4204,    // ONE-SHOT: on kill gain 30% of highest primary stat for 15s
    AA_D_WEAPON_FURY     = 4205,    // Activate: all swings proc weapon on-hit effects for 12s/18s/24s
    AA_D_TWINCAST        = 4206,    // 5%/10%/15% chance spells fire twice at full effectiveness

    // ── ARCHETYPE — HEALER (4301–4307) ─────────────────────
    AA_H_MENDING_TOUCH     = 4301,  // +5%/+10%/+15% healing done from all sources
    AA_H_CRITICAL_HEALING  = 4302,  // +3%/+6%/+10% heal crit chance
    AA_H_SPIRIT_CHANNEL    = 4303,  // +5%/+10%/+15% Spirit
    AA_H_LINGERING_RENEWAL = 4304,  // ONE-SHOT: direct heals leave HoT for 5% of heal over 6s
    AA_H_BATTLE_MENDER     = 4305,  // -5%/-10%/-15% mana cost on healing spells in combat
    AA_H_OVERFLOWING       = 4306,  // Excess healing converts to absorb cap 5%/8%/10% max HP
    AA_H_CHAIN_HEALING     = 4307,  // Heal bounces to lowest HP pet/guardian for 30%/50%/70%

    // ── CLASS — WARRIOR (5001–5018) ────────────────────────
    AA_WAR_RAMPAGE            = 5001,
    AA_WAR_WARCRY             = 5002,
    AA_WAR_TACTICAL_MASTERY   = 5003,
    AA_WAR_LIVING_SHIELD      = 5004,
    AA_WAR_PUNISHING_BLADE    = 5005,
    AA_WAR_REND_MASTERY       = 5006,
    AA_WAR_MORTAL_MASTERY     = 5007,
    AA_WAR_CLEAVING_STRIKES   = 5008,
    AA_WAR_WHIRLWIND_MASTERY  = 5009,
    AA_WAR_SHIELD_MOMENTUM    = 5010,
    AA_WAR_RETALIATION        = 5011,
    AA_WAR_FURIOUS_CHARGE     = 5012,
    AA_WAR_IRON_WARRIOR       = 5013,
    AA_WAR_BATTLE_ENDURANCE   = 5014,
    AA_WAR_RAGE_ENGINE        = 5015,
    AA_WAR_TITANS_GRIP        = 5016,
    AA_WAR_IMPROVED_DEVASTATE = 5017,
    AA_WAR_UNENDING_FURY      = 5018,

    // ── CLASS — PALADIN (5100–5126) ────────────────────────
    AA_PAL_CRUSADERS_MIGHT         = 5100,
    AA_PAL_JUDGE                   = 5101,
    AA_PAL_EXECUTIONER             = 5102,
    AA_PAL_DIVINE_STORM_MASTERY    = 5103,
    AA_PAL_IMPROVED_EXORCISM       = 5104,
    AA_PAL_SERAPHIM                = 5105,
    AA_PAL_MANDATE_OF_HEAVEN       = 5106,
    AA_PAL_HOLY_FORTITUDE          = 5107,
    AA_PAL_RIGHTEOUS_ANGER         = 5108,
    AA_PAL_IMPROVED_CONSECRATION   = 5109,
    AA_PAL_IMPROVED_AVENGERS_SHIELD = 5110,
    AA_PAL_FIST_OF_RECKONING       = 5111,
    AA_PAL_BLESSING_OF_AUSTERITY   = 5112,
    AA_PAL_SANCTUARY               = 5113,
    AA_PAL_IMPROVED_FLASH_OF_LIGHT = 5114,
    AA_PAL_IMPROVED_SEAL_OF_LIGHT  = 5115,
    AA_PAL_QUICK_BUFF              = 5116,
    AA_PAL_LAY_OF_HANDS_MASTERY    = 5117,
    AA_PAL_HOLY_WRATH_MASTERY      = 5118,
    AA_PAL_FEARLESS                = 5119,
    AA_PAL_YAULP                   = 5120,
    AA_PAL_CELESTIAL_REGENERATION  = 5121,
    AA_PAL_CELESTIAL_HAMMER        = 5122,
    AA_PAL_GIFT_OF_THE_KEEPER      = 5123,
    AA_PAL_DIVINE_PROVIDENCE       = 5124,
    AA_PAL_PURIFYING_JUDGMENT      = 5125,
    AA_PAL_UNYIELDING_LIGHT        = 5126,

    // ── CLASS — HUNTER (5201–5248) ─────────────────────────
    AA_HUN_ARCHERY_MASTERY        = 5201,
    AA_HUN_DOUBLE_BOWSHOT         = 5202,
    AA_HUN_ENDLESS_QUIVER         = 5203,
    AA_HUN_HEADSHOT               = 5204,
    AA_HUN_TRIPLE_ARROW           = 5205,
    AA_HUN_EXPLOSIVE_ARROW        = 5206,
    AA_HUN_VOLLEY_BURST           = 5207,
    AA_HUN_INNATE_CAMOUFLAGE      = 5208,
    AA_HUN_VEIL_OF_MINDSHADOW     = 5209,
    AA_HUN_NATURES_GUIDANCE       = 5210,
    AA_HUN_ENTRAP                 = 5211,
    AA_HUN_WIND_OF_THE_SOUTH      = 5212,
    AA_HUN_AUSPICE                = 5213,
    AA_HUN_SNARING_SHOT           = 5214,
    AA_HUN_POISON_ARROW           = 5215,
    AA_HUN_BURNING_ARROW          = 5216,
    AA_HUN_TASTE_OF_BLOOD         = 5217,
    AA_HUN_SCOUT_OF_THE_WILD      = 5218,
    AA_HUN_EAGLE_EYE              = 5219,
    AA_HUN_HARMONIOUS_ARROW       = 5220,
    AA_HUN_NATURES_MELODY         = 5221,
    AA_HUN_CALL_OF_THE_WILD       = 5222,
    AA_HUN_PATHFINDING            = 5223,
    AA_HUN_CAREFUL_AIM            = 5224,
    AA_HUN_QUICK_RECOVERY         = 5225,
    AA_HUN_IMPROVED_SHOTS         = 5226,
    AA_HUN_ARCANE_QUIVER          = 5227,
    AA_HUN_CHEER_DEFENSIVE        = 5228,
    AA_HUN_IMPROVED_BLACK_ARROW   = 5229,
    AA_HUN_OBSIDIAN_ARROWS        = 5230,
    AA_HUN_ENCHANTED_ARROWS       = 5231,
    AA_HUN_VOLATILE_ENERGIES      = 5232,
    AA_HUN_COMPANION_BOND         = 5233,
    AA_HUN_PET_ATTUNEMENT         = 5234,
    AA_HUN_NATURAL_GRACE          = 5235,
    AA_HUN_NETHER_RAY_STING       = 5236,
    AA_HUN_POISON_GAS             = 5237,
    AA_HUN_ARCANE_ANOMALY         = 5238,
    AA_HUN_IMPROVED_BESTIAL_WRATH = 5239,
    AA_HUN_EXPLOSIVE_CHARGE       = 5240,
    AA_HUN_FOCUSED_BARRAGE        = 5241,
    AA_HUN_MARKED_FOR_DEATH       = 5242,
    AA_HUN_CHEER_OFFENSIVE        = 5243,
    AA_HUN_CHEER_SWIFTNESS        = 5244,
    AA_HUN_DEDICATION             = 5245,
    AA_HUN_RANGED_MASTERY         = 5246,
    AA_HUN_IMPROVED_TRAPS         = 5247,
    AA_HUN_THRILL_OF_THE_HUNT     = 5248,
    AA_HUN_STEADY_FOCUS           = 5249,  // +60/120/200 ranged attack power
    AA_HUN_PIERCING_ROUNDS        = 5250,  // +84 armor pen rating per rank
    AA_HUN_SURVIVAL_TACTICS       = 5251,  // +45/90/160 dodge rating per rank
    AA_HUN_BEAST_SYNERGY          = 5252,  // +3/6/10% damage while pet/guardian alive
    AA_HUN_COORDINATED_ASSAULT    = 5253,  // +5/10/15% damage to target pet is attacking
    AA_HUN_GO_FOR_THE_THROAT      = 5254,  // 10/20/30% proc: pet bites for 40/60/80% of hit

    // ── CLASS — ROGUE (5301–5341) ───────────────────────────
    AA_ROG_AMBIDEXTERITY   = 5301,
    AA_ROG_BACKSTAB_FOCUS  = 5302,
    AA_ROG_DEATH_BLOW      = 5303,
    AA_ROG_CHEAP_SHOT      = 5304,
    AA_ROG_ESCAPE_ARTIST   = 5305,
    AA_ROG_FLURRY          = 5306,
    AA_ROG_HASTENED_ATTACKS = 5307,
    AA_ROG_LEG_HOLD        = 5308,
    AA_ROG_POISON_MASTERY  = 5309,
    AA_ROG_QUICK_STRIKE    = 5310,
    AA_ROG_PUNCTURE        = 5311,
    AA_ROG_SHADOW_WALK     = 5312,
    AA_ROG_SPEED_OF_SHADOWS = 5313,
    AA_ROG_SUBLIMATION     = 5314,
    AA_ROG_ASSASSINS_MARK  = 5315,
    AA_ROG_LACERATE        = 5316,
    AA_ROG_DANCING_BLADE   = 5317,
    AA_ROG_FRENZY          = 5318,
    AA_ROG_TRIP            = 5319,
    AA_ROG_SLIPPERY        = 5320,
    AA_ROG_INGENUITY       = 5321,
    AA_ROG_TRAUMA          = 5322,
    AA_ROG_WEAK_SPOT       = 5323,
    AA_ROG_TRICKS          = 5324,
    AA_ROG_BLEEDING_FLURRY = 5325,
    AA_ROG_KILLING_SPREE   = 5326,
    AA_ROG_IMP_HUNGER_FOR_BLOOD = 5327,
    AA_ROG_BLADE_FLURRY    = 5328,
    AA_ROG_VANISH_CLONE    = 5329,
    AA_ROG_IMP_RUPTURE     = 5330,
    AA_ROG_DEFLECTION      = 5331,
    AA_ROG_DEBILITATION    = 5332,
    AA_ROG_HACK_AND_SLASH  = 5333,
    AA_ROG_IMP_PREMEDITATION = 5334,
    AA_ROG_IMP_RIPOSTE     = 5335,
    AA_ROG_IMP_MUTILATE    = 5336,
    AA_ROG_DUPLICITY       = 5337,
    AA_ROG_INVIGORATION    = 5338,
    AA_ROG_POISON_MASTER   = 5339,
    AA_ROG_SHADOWSTEP_MASTERY = 5340,
    AA_ROG_CHAOTIC_STAB    = 5341,

    // ── CLASS — PRIEST (5401–5446) ──────────────────────────
    AA_PRI_TWINHEAL              = 5401,
    AA_PRI_GIFT_OF_MANA          = 5402,
    AA_PRI_CHANNELING_DIVINE     = 5403,
    AA_PRI_FORCEFUL_REJUVENATION = 5404,
    AA_PRI_YAULP                 = 5405,
    AA_PRI_CELESTIAL_HAMMER      = 5406,
    AA_PRI_CELESTIAL_REGEN       = 5407,
    AA_PRI_PROLONGED_SALVE       = 5408,
    AA_PRI_QUICK_BUFF            = 5409,
    AA_PRI_PERSISTENT_CASTING    = 5410,
    AA_PRI_LASTING_RITES         = 5411,
    AA_PRI_FORCE_OF_WILL         = 5412,
    AA_PRI_DIVINE_STUN           = 5413,
    AA_PRI_MARK_OF_KARNA         = 5414,
    AA_PRI_INVOCATION            = 5415,
    AA_PRI_TURN_UNDEAD           = 5416,
    AA_PRI_RADIANT_CURE          = 5417,
    AA_PRI_DIVINE_ARBITRATION    = 5418,
    AA_PRI_ARMOR_OF_WISDOM       = 5419,
    AA_PRI_CELESTIAL_BARRIER     = 5420,
    AA_PRI_BESTOW_DIVINE_AURA    = 5421,
    AA_PRI_SPIRITUAL_LIGHT       = 5422,
    AA_PRI_TOUCH_OF_THE_DIVINE   = 5423,
    AA_PRI_SANCTIFICATION        = 5424,
    AA_PRI_AURA_OF_PIOUS         = 5425,
    AA_PRI_WAKE_OF_TRANQUILITY   = 5426,
    AA_PRI_IMP_POWER_INFUSION    = 5427,
    AA_PRI_SPREADING_MISERY      = 5428,
    AA_PRI_EMPOWERED_HOLY_NOVA   = 5429,
    AA_PRI_CHAIN_REACTION        = 5430,
    AA_PRI_HARBINGER             = 5431,
    AA_PRI_PERSISTENCE           = 5432,
    AA_PRI_ENCROACHING_DARKNESS  = 5433,
    AA_PRI_SHADOW_ERUPTION       = 5434,
    AA_PRI_DISCIPLE_OF_CTHUN     = 5435,
    AA_PRI_WANDERING_SPIRITS     = 5436,
    AA_PRI_DIVINE_GUARDIAN       = 5437,
    AA_PRI_SHARED_LIFE           = 5438,
    AA_PRI_IMP_PRAYER_OF_MENDING = 5439,
    AA_PRI_INSPIRE               = 5440,
    AA_PRI_IMP_BODY_AND_SOUL     = 5441,
    AA_PRI_IMP_LIGHTWELL         = 5442,
    AA_PRI_DIVINE_PURPOSE        = 5443,
    AA_PRI_GUARDIAN_ANGEL        = 5444,
    AA_PRI_IMP_SHIELD            = 5445,
    AA_PRI_PENANCE_MASTERY       = 5446,

    // ── CLASS — DEATH KNIGHT (5501–5526) ───────────────────
    AA_DK_PLAGUE_LORD         = 5501,
    AA_DK_PESTILENCE          = 5502,
    AA_DK_LIFEBURN            = 5503,
    AA_DK_BLOOD_RITE          = 5504,
    AA_DK_UNHOLY_GUARD        = 5505,  // PENDING: RP-spend requires DK as first class
    AA_DK_NECROTIC_TOUCH      = 5506,
    AA_DK_IRON_SHELL          = 5507,
    AA_DK_FROST_ROT           = 5508,
    AA_DK_DEATH_PACT          = 5509,
    AA_DK_CONTAGION_DRAIN     = 5510,
    AA_DK_RUNE_AWAKENING      = 5511,
    AA_DK_DEATHS_HUNGER       = 5512,
    AA_DK_SCOURGE_MASTERY     = 5513,
    AA_DK_RUNE_BLADE          = 5514,
    AA_DK_ARCTIC_HOWL         = 5515,
    AA_DK_BATTLE_FRENZY       = 5516,
    AA_DK_DEATHCHILL          = 5517,
    AA_DK_PLAGUES_END         = 5518,
    AA_DK_FINAL_RUNE          = 5519,
    AA_DK_VIRULENT_PLAGUE     = 5520,
    AA_DK_GHOUL_INFESTATION   = 5521,
    AA_DK_DETONATION          = 5522,
    AA_DK_ARMY_COMMANDER      = 5523,
    AA_DK_SOUL_ABRASION       = 5524,
    AA_DK_LEECH_TOUCH         = 5525,
    AA_DK_IMPROVED_HARM_TOUCH = 5526,

    // ── CLASS — SHAMAN (5601–5621) ──────────────────────────
    AA_SHA_CANNIBALIZE       = 5601,
    AA_SHA_BLOOD_TITHE       = 5602,
    AA_SHA_EARTHEN_PRESENCE  = 5603,
    AA_SHA_TOTEMIC_MASTERY   = 5604,
    AA_SHA_ANCESTRAL_GUARD   = 5605,
    AA_SHA_WINDLORD          = 5606,
    AA_SHA_WEAPON_ATTUNEMENT = 5607,
    AA_SHA_SHOCK_RESONANCE   = 5608,
    AA_SHA_SOUL_HARVEST      = 5609,
    AA_SHA_ALPHA_PACK        = 5610,
    AA_SHA_SPIRIT_BOND       = 5611,
    AA_SHA_THUNDEROUS_STRIKE = 5612,
    AA_SHA_LAVA_SURGE        = 5613,
    AA_SHA_SCORCHED_EARTH    = 5614,
    AA_SHA_LIGHTNING_ROD     = 5615,
    AA_SHA_MAELSTROM_MASTERY = 5616,
    AA_SHA_GHOST_STRIKE      = 5617,
    AA_SHA_SWIFT_CURRENT     = 5618,
    AA_SHA_LIVING_CURRENT    = 5619,
    AA_SHA_ELEMENTAL_ACCORD  = 5620,
    AA_SHA_ELEMENTAL_FURY    = 5621,

    // ── CLASS — MAGE (5700–5740) ────────────────────────────
    AA_MAG_SHORT_FUSE            = 5700,
    AA_MAG_EXPLOSIVE_IMPACT      = 5701,
    AA_MAG_SPREADING_FLAMES      = 5702,
    AA_MAG_EMPOWERED_FLAMES      = 5703,
    AA_MAG_METEOR_STRIKE         = 5704,
    AA_MAG_DRAGONS_FIRE          = 5705,
    AA_MAG_METEOR_SHOWER         = 5706,
    AA_MAG_SLOW_BURN             = 5707,
    AA_MAG_IMPROVED_FROSTBOLT    = 5708,
    AA_MAG_AUGMENTED_DEEP_FREEZE = 5709,
    AA_MAG_IMPROVED_DEEP_FREEZE  = 5710,
    AA_MAG_IMPROVED_FROST_WARD   = 5711,
    AA_MAG_IMPROVED_ICE_BARRIER  = 5712,
    AA_MAG_AUGMENTED_ICY_VEINS   = 5713,
    AA_MAG_ARCANE_BOMBARDMENT    = 5714,
    AA_MAG_ARCANE_SUBTLETY       = 5715,
    AA_MAG_CHAIN_EXPLOSION       = 5716,
    AA_MAG_ARCANE_ATTUNEMENT     = 5717,
    AA_MAG_FOCUSED_MAGIC         = 5718,
    AA_MAG_LOST_IN_TIME          = 5719,
    AA_MAG_MANA_BATTERY          = 5720,
    AA_MAG_ARCANE_PRESENCE       = 5721,
    AA_MAG_FLAMEBRINGER          = 5722,
    AA_MAG_ILLUSION_OF_CHOICE    = 5723,
    AA_MAG_SLIPPERY_SLOPE        = 5724,
    AA_MAG_MIRRORED_DEFENSE      = 5725,
    AA_MAG_OPTICAL_ILLUSION      = 5726,
    AA_MAG_HIVEMIND              = 5727,
    AA_MAG_HALLUCINATIONS        = 5728,
    AA_MAG_QUICK_DAMAGE          = 5729,
    AA_MAG_HARVEST_OF_DRUZZIL    = 5730,
    AA_MAG_MANABURN              = 5731,
    AA_MAG_SPELL_CASTING_SUBTLETY = 5732,
    AA_MAG_CALL_OF_XUZL          = 5733,
    AA_MAG_IMPROVED_FAMILIAR     = 5734,
    AA_MAG_FRENZIED_BURNOUT      = 5735,
    AA_MAG_MEND_COMPANION        = 5736,
    AA_MAG_QUICK_SUMMONING       = 5737,
    AA_MAG_HOST_OF_THE_ELEMENTS  = 5738,
    AA_MAG_DESTRUCTIVE_FURY      = 5739,
    AA_MAG_CHAOTIC_FEEDBACK      = 5740,

    // ── CLASS — WARLOCK (5800–5835) ─────────────────────────
    AA_WRL_THREADS_OF_DESPAIR    = 5800,
    AA_WRL_MORTAL_ERADICATION    = 5801,
    AA_WRL_IMPROVED_DRAINS       = 5802,
    AA_WRL_IMPROVED_DRAIN_LIFE   = 5803,
    AA_WRL_IMPROVED_DRAIN_MANA   = 5804,
    AA_WRL_IMPROVED_DRAIN_SOUL   = 5805,
    AA_WRL_IMPROVED_CURSES       = 5806,
    AA_WRL_SPIRIT_LASH           = 5807,
    AA_WRL_UMBRAL_LEECH          = 5808,
    AA_WRL_BURNING_SOUL          = 5809,
    AA_WRL_MOLTEN_SKIN           = 5810,
    AA_WRL_BACKDRAFT             = 5811,
    AA_WRL_CRITICAL_MASS         = 5812,
    AA_WRL_EMBERSTORM            = 5813,
    AA_WRL_TENEBROUS_REACH       = 5814,
    AA_WRL_DESTRUCTIVE_PATH      = 5815,
    AA_WRL_NETHER_PORTAL         = 5816,
    AA_WRL_INFERNAL_VOLCANO      = 5817,
    AA_WRL_DEMONIC_SYNERGY       = 5818,
    AA_WRL_EMPOWERED_DEMONS      = 5819,
    AA_WRL_DEMONIC_KNOWLEDGE     = 5820,
    AA_WRL_WELL_OF_SOULS         = 5821,
    AA_WRL_SOUL_BARRAGE          = 5822,
    AA_WRL_SOULSTORM             = 5823,
    AA_WRL_CIRCLE_OF_THE_DAMNED  = 5824,
    AA_WRL_SOUL_MIRROR           = 5825,
    AA_WRL_WAKE_THE_DEAD         = 5826,
    AA_WRL_FEARSTORM             = 5827,
    AA_WRL_LIFEBURN              = 5828,
    AA_WRL_DIRE_CHARM            = 5829,
    AA_WRL_SOUL_ABRASION         = 5830,
    AA_WRL_LEECH_TOUCH           = 5831,
    AA_WRL_IMPROVED_HARM_TOUCH   = 5832,
    AA_WRL_SUSPENDED_MINION      = 5833,
    AA_WRL_FEIGNED_MINION        = 5834,
    AA_WRL_SPELL_CASTING_SUBTLETY = 5835,

    // ── CLASS — DRUID (5900–5934) — ALL DEFERRED (mod-druid-essence required) ──
    AA_DRU_IMP_LACERATE_RAKE         = 5900,
    AA_DRU_RIP_AND_TEAR              = 5901,
    AA_DRU_BEAST_WITHIN              = 5902,
    AA_DRU_IMPROVED_BEAST_FORM       = 5903,
    AA_DRU_AUGMENTED_BEAST_FORM      = 5904,
    AA_DRU_IMPROVED_BERSERK          = 5905,
    AA_DRU_IMPROVED_FAERIE_FIRE      = 5906,
    AA_DRU_WRATH_OF_THE_WILD         = 5907,
    AA_DRU_NATURES_TENACITY          = 5908,
    AA_DRU_NATURES_ALACRITY          = 5909,
    AA_DRU_CELESTIAL_IMPACT          = 5910,
    AA_DRU_CELESTIAL_WRATH           = 5911,
    AA_DRU_ANCESTRAL_SPIRITS         = 5912,
    AA_DRU_IMPROVED_TYPHOON          = 5913,
    AA_DRU_QUICK_DAMAGE              = 5914,
    AA_DRU_DESTRUCTIVE_FURY          = 5915,
    AA_DRU_NATURES_CHOSEN            = 5916,
    AA_DRU_SPIRIT_OF_THE_WOOD        = 5917,
    AA_DRU_HEALING_ADEPT             = 5918,
    AA_DRU_HEALING_GIFT              = 5919,
    AA_DRU_NATURES_REMEDY            = 5920,
    AA_DRU_SPELL_CASTING_REINFORCEMENT = 5921,
    AA_DRU_PACK_CHLOROPLAST          = 5922,
    AA_DRU_SWIFTMEND_MASTERY         = 5923,
    AA_DRU_RADIANT_CURE              = 5924,
    AA_DRU_DIRE_CHARM                = 5925,
    AA_DRU_INNATE_CAMOUFLAGE         = 5926,
    AA_DRU_ENHANCED_ROOT             = 5927,
    AA_DRU_IMPROVED_THORNS           = 5928,
    AA_DRU_AUGMENTED_THORNS          = 5929,
    AA_DRU_GROVE_TRAP_SPORE_BLOOM    = 5930,
    AA_DRU_GROVE_TRAP_BURST_BLOOM    = 5931,
    AA_DRU_GROVE_TRAP_LIGHTNING_BLOOM = 5932,
    AA_DRU_GROVE_TRAP_THORN_FLAYER   = 5933,
    AA_DRU_CHAOTIC_STAB              = 5934,

    // ── LEGACY / STARTER ────────────────────────────────────
    AA_TEMPER = 9001,               // Free at creation — enables .armoryslot temper
};

// ---------------------------------------------------------------------------
// Runtime query API — safe to call from any script hook.
// GetRank returns 0 if the player does not have the AA or is nullptr.
// ---------------------------------------------------------------------------
namespace SanctumAA
{
    uint8 GetRank(Player const* player, uint32 aaId);

    inline bool Has(Player const* player, uint32 aaId, uint8 minRank = 1)
    {
        return GetRank(player, aaId) >= minRank;
    }
}
