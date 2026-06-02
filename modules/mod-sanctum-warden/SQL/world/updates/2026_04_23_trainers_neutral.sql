-- Sanctum: Set all trainer NPCs to faction 35 (Friendly to all).
--
-- Root cause: Trainer NPCs retain their original Horde/Alliance faction.
-- Cross-faction players (e.g. Human visiting Orgrimmar) cannot interact with
-- Horde trainers even though guards are neutral, because the trainers still
-- perform a faction/reputation check on interaction.
--
-- Fix: Set faction = 35 on any NPC whose npcflag includes a trainer bit.
--   UNIT_NPC_FLAG_TRAINER            = 0x00000010 (16)
--   UNIT_NPC_FLAG_TRAINER_CLASS      = 0x00000020 (32)
--   UNIT_NPC_FLAG_TRAINER_PROFESSION = 0x00000040 (64)
--
-- The actual spell-availability check (player must be the right class to learn
-- class spells) still runs server-side — this only removes the faction gate.
-- Safe to run multiple times.

UPDATE creature_template
SET faction = 35
WHERE (npcflag & 16 > 0 OR npcflag & 32 > 0 OR npcflag & 64 > 0)
  AND flags_extra & 2 = 0;  -- exclude Civilian-flagged NPCs
