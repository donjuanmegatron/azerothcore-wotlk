-- mod-sanctum-warden world SQL
-- Sanctum Warden — New Character Experience
--
-- Runs automatically against acore_world on worldserver start.
--
-- This file:
--   1. Spawns all new characters in Dalaran (all races, all classes)
--   2. Creates the Sanctum Warden NPC (entry 700200)
--   3. Sets city guard factions to 35 (neutral — faction is irrelevant in Sanctum)
--
-- Dalaran coordinates (map 571, Krasus' Landing area, near central fountain):
--   X = 5804.15,  Y = 624.77,  Z = 648.09,  O = 1.57 (facing north)
--   zone = 4395 (Dalaran)

-- ============================================================
-- 1. Spawn ALL new characters in Dalaran
--
-- This overwrites the per-race starting positions so every character
-- (regardless of race) appears in Dalaran on first login and meets
-- the Sanctum Warden.  The Warden then teleports them to their
-- chosen starting zone after class selection is complete.
--
-- The mod-dk-rework SQL sets the same Dalaran coords for class 6 only.
-- This sets it for everyone, so no WHERE clause is needed.
-- ============================================================

UPDATE `playercreateinfo`
SET
    `map`         = 571,
    `zone`        = 4395,
    `position_x`  = 5804.15,
    `position_y`  = 624.77,
    `position_z`  = 648.09,
    `orientation` = 1.57;

-- ============================================================
-- 2. Sanctum Warden NPC — dialogue text (npc_text)
-- ============================================================

DELETE FROM `npc_text` WHERE `ID` IN (700200, 700201, 700202, 700203, 700204, 700205, 700206);

INSERT INTO `npc_text` (`ID`, `text0_0`) VALUES
(700200, 'Greetings, $N. I am the Sanctum Warden.\n\nBefore you venture into the world, you must bind two additional classes to your soul. Choose carefully — these choices are permanent.\n\nWhen your classes are bound, I will send you to the starting zone of your choice.'),
(700201, 'Choose your second class. You will gain all of their spells and abilities immediately.'),
(700202, 'Your second class is bound. Now choose your third and final class.'),
(700203, 'Your three classes are sealed. Choose the land where your journey begins.\n\nFaction does not bind you — any zone is open to you.'),
(700204, 'This choice is permanent. Once bound, a class cannot be removed.\n\nAre you certain?'),
(700205, 'Your path is sealed. Go forth.'),
(700206, 'Your three classes are bound. I can train all your class abilities whenever you need.');

-- ============================================================
-- 3. Sanctum Warden NPC template (creature_template)
--
-- entry   700200 = Sanctum Warden
-- npcflag 1      = UNIT_NPC_FLAG_GOSSIP (right-click opens dialogue)
-- faction 35     = Friendly to all players
-- flags_extra 2  = Civilian (will not attack)
-- ScriptName must match the CreatureScript name in the .cpp file
-- ============================================================

DELETE FROM `creature_template` WHERE `entry` = 700200;

INSERT INTO `creature_template`
    (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`,
     `npcflag`, `speed_walk`, `speed_run`, `unit_class`,
     `type`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
    (700200, 'Sanctum Warden', 'New Adventurer Guide', 80, 80, 2, 35,
     1, 1.0, 1.14286, 1,
     7, 1, 2, 'npc_sanctum_warden');

-- NPC display model: Archmage appearance (same robed caster used for Class Weaver)
DELETE FROM `creature_template_model` WHERE `CreatureID` = 700200;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`)
VALUES (700200, 0, 1595, 1.0, 1.0);

-- ============================================================
-- 4. Sanctum Warden spawn — Dalaran only
--
-- All characters start in Dalaran, so one centrally placed Warden
-- is enough.  Positioned near the central fountain where new
-- characters will arrive.
-- ============================================================

-- ============================================================
-- 4. Sanctum Warden spawns — Dalaran + all capital cities
--
-- One Warden in every major city so players can train class
-- spells at any level without hunting for individual class trainers.
--
-- Coordinates are approximate city-center positions.
-- Use .npc move in-game if any spawn needs adjustment.
-- ============================================================

DELETE FROM `creature` WHERE `id1` = 700200;

INSERT INTO `creature` (`id1`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`)
VALUES
    -- Dalaran (WotLK floating city) — new character starting point
    (700200, 571,  5804.15,   624.77,   648.09, 4.71, 300),
    -- Stormwind — Cathedral Square
    (700200,   0, -8803.45,   617.05,    97.89, 1.57, 300),
    -- Orgrimmar — Valley of Strength
    (700200,   1,  1599.17, -4374.16,    16.05, 5.49, 300),
    -- Ironforge — The Commons
    (700200,   0, -4786.77, -1118.16,   501.28, 0.00, 300),
    -- Darnassus — Craftsmen's Terrace
    (700200,   1,  9951.46,  2280.07,  1341.39, 3.14, 300),
    -- Thunder Bluff — High Rise (central bluff)
    (700200,   1, -1259.98,    66.47,   128.31, 0.00, 300),
    -- Undercity — The Trade Quarter
    (700200,   0,  1567.61,   237.87,   -62.00, 4.71, 300),
    -- The Exodar — The Crystal Hall
    (700200, 530, -3897.40, -11718.53,   -2.70, 0.00, 300),
    -- Silvermoon City — The Royal Exchange
    (700200, 530, 10132.40,  -6801.14,    16.51, 1.57, 300);

-- ============================================================
-- 5. Faction neutrality — city guards AND all starting-zone civilians
--
-- Sets creature_template faction to 35 (Friendly to all) for every
-- NPC faction that is hostile to the opposite player faction.
-- This lets any race enter any city or starting zone safely.
--
-- City guards (identified by faction research):
--   11   = Stormwind Guard
--   57   = Ironforge Guard
--   71   = Undercity Guardian
--   79   = Darnassus Sentinel
--   85   = Orgrimmar Grunt
--   104  = Thunder Bluff Watcher
--   1603 = Silvermoon Guardian
--   1639 = Exodar Peacekeeper
--   1758 = Orgrimmar Grunt (warchief variant)
--
-- Horde starting-zone civilians (confirmed hostile to Alliance via
-- factiontemplate_dbc.EnemyGroup & 2):
--   29   = Darkspear Troll civilians (Valley of Trials)
--   68   = Forsaken civilians (Deathknell)
--   83   = Tauren civilians (Red Cloud Mesa / Mulgore)
--   105  = Thunder Bluff Protectors
--   126  = Darkspear Troll variant (Troll starting NPCs)
--   876  = Horde starting zone NPC variant
--   1604 = Sunstrider Isle / Blood Elf civilians (Eversong Woods)
--
-- This affects creature_template rows only.
-- ============================================================

UPDATE `creature_template`
SET `faction` = 35
WHERE `faction` IN (11, 29, 57, 68, 71, 79, 83, 85, 104, 105, 126, 876, 1603, 1604, 1639, 1758);

-- ============================================================
-- 6. Cross-faction questing — remove race restrictions
--
-- AllowableRaces is a bitmask that limits which races can accept
-- each quest.  Setting it to 0 means no restriction — any race
-- can take the quest.  This lets Alliance characters accept Horde
-- quests and vice versa.
-- ============================================================

UPDATE `quest_template`
SET `AllowableRaces` = 0
WHERE `AllowableRaces` > 0;

-- ============================================================
-- 7. Universal NPC language
--
-- broadcast_text.LanguageID controls which language NPCs "speak".
-- Non-zero values make their text appear as "???" to players who
-- don't know that language (e.g., Orcish text shows ??? to Alliance).
-- Setting all to 0 (Universal) makes every NPC readable by every
-- player regardless of race or faction.
-- ============================================================

UPDATE `broadcast_text`
SET `LanguageID` = 0
WHERE `LanguageID` != 0;
