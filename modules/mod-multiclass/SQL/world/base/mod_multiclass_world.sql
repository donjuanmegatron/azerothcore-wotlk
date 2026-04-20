-- mod-multiclass world database SQL
-- Runs automatically against acore_world on worldserver start.
-- Class selection is handled by the Sanctum Warden (mod-sanctum-warden).
-- This file only manages the multiclass trainer script on class trainer NPCs.

-- ============================================================
-- Multiclass Talent Storage
--
-- Stores talent spending for secondary and tertiary class trees.
-- Primary class (class1) talent spending is tracked by WoW's
-- native character_talent system.
-- class_id: WoW class ID (1-11) for the secondary/tertiary class
-- talent_id: TalentEntry.TalentID from Talent.dbc
-- rank: current rank spent (1 to maxRank)
-- ============================================================

CREATE TABLE IF NOT EXISTS character_multiclass_talents (
    guid     INT UNSIGNED    NOT NULL,
    class_id TINYINT UNSIGNED NOT NULL,
    talent_id INT UNSIGNED   NOT NULL,
    `rank`   TINYINT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (guid, class_id, talent_id)
);

-- ============================================================
-- Multiclass Trainer Script
--
-- Assigns ScriptName 'npc_multiclass_trainer' to all class trainer
-- NPCs so that the MulticlassTrainerScript gossip handler fires
-- when any player right-clicks them.
--
-- Only updates NPCs that:
--   - Are linked to a class trainer (trainer.Type = 0)
--   - Have a class requirement (trainer.Requirement > 0)
--   - Do not already have a custom ScriptName
-- ============================================================

UPDATE `creature_template` ct
INNER JOIN `creature_default_trainer` cdt ON cdt.CreatureId = ct.entry
INNER JOIN `trainer` t ON t.Id = cdt.TrainerId
SET ct.`ScriptName` = 'npc_multiclass_trainer'
WHERE t.`Type` = 0
  AND t.`Requirement` > 0
  AND (ct.`ScriptName` = '' OR ct.`ScriptName` IS NULL);

-- ============================================================
-- Fix: some generic class trainers (entries 26324-26332) have
-- npcflag = 48 (TRAINER + TRAINER_CLASS) with no GOSSIP flag (1).
-- Without the gossip flag the client never sends CMSG_GOSSIP_HELLO,
-- so OnGossipHello is never called and our script is bypassed.
-- This adds UNIT_NPC_FLAG_GOSSIP (1) to every trainer we assigned
-- the script to that was missing it.
-- ============================================================

UPDATE `creature_template`
SET `npcflag` = `npcflag` | 1
WHERE `ScriptName` = 'npc_multiclass_trainer'
  AND (`npcflag` & 1) = 0;

-- ============================================================
-- Druid Essence Aura Spells (server-side)
--
-- These are SPELL_AURA_DUMMY spells applied when a Druid presses
-- a combat form.  The client knows about these via patch-5.MPQ.
--
-- Visual IDs reference existing SpellVisual.dbc entries:
--   Bear Essence    7553  = Power Infusion (blue shimmer)
--   Cat Essence     11567 = Eclipse Solar  (golden energy)
--   Moonkin Essence 12705 = Eclipse Lunar  (arcane purple)
--   Tree Essence    3884  = Innervate      (green wisps)
-- ============================================================

REPLACE INTO `spell_dbc`
    (`ID`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`,
     `Effect_1`, `EffectAura_1`, `ImplicitTargetA_1`,
     `SpellVisualID_1`, `SpellIconID`,
     `Name_Lang_enUS`, `Name_Lang_Mask`)
VALUES
    (900001, 1, 21, 1, 6, 4, 1, 7553,  107,  'Bear Essence',    4),
    (900002, 1, 21, 1, 6, 4, 1, 11567, 493,  'Cat Essence',     4),
    (900003, 1, 21, 1, 6, 4, 1, 12705, 111,  'Moonkin Essence', 4),
    (900004, 1, 21, 1, 6, 4, 1, 3884,  2257, 'Tree Essence',    4);

-- ============================================================
-- Druid Essence SpellScript bindings
--
-- Binds 'spell_druid_essence_form' SpellScript to the five
-- Druid shapeshift spells so the script intercepts them and
-- applies an essence aura instead of transforming the model.
-- ============================================================

INSERT IGNORE INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(5487,  'spell_druid_essence_form'),
(9634,  'spell_druid_essence_form'),
(768,   'spell_druid_essence_form'),
(24858, 'spell_druid_essence_form'),
(33891, 'spell_druid_essence_form');
