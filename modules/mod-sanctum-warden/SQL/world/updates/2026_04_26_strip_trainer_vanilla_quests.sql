-- Strip all vanilla quest associations from Class Master NPCs (2026-04-26)
--
-- Every NPC assigned ScriptName='npc_multiclass_trainer' is a Sanctum Class Master.
-- These are vanilla NPC entries repurposed as class trainers. Their original
-- creature_queststarter / creature_questender rows cause vanilla starting-zone
-- quests to appear on (or be turned in at) our Class Masters, creating stuck
-- quest states for players who pick racial starting zones.
--
-- This DELETE severs that connection entirely. The quests still exist in the DB
-- but no NPC will offer or accept them.

DELETE FROM creature_queststarter
WHERE id IN (SELECT entry FROM creature_template WHERE ScriptName = 'npc_multiclass_trainer');

DELETE FROM creature_questender
WHERE id IN (SELECT entry FROM creature_template WHERE ScriptName = 'npc_multiclass_trainer');
