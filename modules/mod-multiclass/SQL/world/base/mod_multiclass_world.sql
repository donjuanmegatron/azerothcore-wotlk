-- mod-multiclass world database SQL
-- Runs automatically against acore_world on worldserver start.
-- Creates the Class Weaver NPC template and spawns it in three locations.

-- ============================================================
-- NPC Dialogue Text (npc_text)
-- These are the words the NPC "says" in the gossip window header.
-- ============================================================

DELETE FROM `npc_text` WHERE `ID` IN (700100, 700101, 700102, 700103, 700104);

INSERT INTO `npc_text` (`ID`, `text0_0`) VALUES
(700100, 'Greetings, $N. I am the Class Weaver. Through me you may bind two additional classes to your soul.\n\nChoose carefully — once bound, a class cannot be removed.'),
(700101, 'Choose your second class. You will gain all of their spells and abilities.'),
(700102, 'Your second class is bound. Now choose your third and final class.'),
(700103, 'Your three classes are bound for eternity. You walk a path no other can follow.'),
(700104, 'This choice is permanent. Are you certain?');

-- ============================================================
-- NPC Template (creature_template)
-- Entry 700100 = Class Weaver
-- npcflag 1    = UNIT_NPC_FLAG_GOSSIP (right-click opens dialogue)
-- faction 35   = Friendly to all players regardless of race/faction
-- flags_extra 2 = Civilian (will not attack the player)
-- ScriptName must match the CreatureScript name in mod-multiclass.cpp
-- ============================================================

DELETE FROM `creature_template` WHERE `entry` = 700100;

INSERT INTO `creature_template`
    (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`,
     `npcflag`, `speed_walk`, `speed_run`, `modelid1`, `unit_class`,
     `type`, `InhabitType`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
    (700100, 'Class Weaver', 'Sanctum Class Master', 80, 80, 2, 35,
     1, 1.0, 1.14286, 14441, 1,
     7, 3, 1, 2, 'npc_class_weaver');

-- ============================================================
-- NPC Spawns (creature)
-- Three spawn points: Dalaran, Stormwind, Orgrimmar.
-- Map 571 = Dalaran (WotLK floating city — neutral, accessible to all)
-- Map 0   = Eastern Kingdoms (Stormwind)
-- Map 1   = Kalimdor (Orgrimmar)
--
-- NOTE: If `id1` causes an error on your AzerothCore version, change it to `id`.
-- ============================================================

DELETE FROM `creature` WHERE `id1` = 700100;

INSERT INTO `creature` (`id1`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`)
VALUES
    (700100, 571,  5804.15,  625.33, 647.77, 1.57, 300),  -- Dalaran, near central fountain
    (700100, 0,   -8791.45,  632.88,  98.27, 0.00, 300),  -- Stormwind, Cathedral Square
    (700100, 1,    1653.71, -4398.26,  16.06, 3.14, 300); -- Orgrimmar, Valley of Strength
