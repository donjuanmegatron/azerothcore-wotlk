-- mod-multiclass world database SQL
-- Runs automatically against acore_world on worldserver start.
-- Class selection is handled by the Sanctum Warden (mod-sanctum-warden).
-- This file only manages the multiclass trainer script on class trainer NPCs.

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
