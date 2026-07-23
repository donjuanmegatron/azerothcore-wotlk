-- mod-companion-system PHASE 1: "The Band" roster NPCs
-- ---------------------------------------------------------------------------
-- Four named companion NPCs (mod-companion-system.cpp, ScriptName
-- npc_sanctum_companion, shared by all four). Idle in the hub with
-- npcflag=1 (gossip): "Come with me." / "Head back to Sanctum." / "Call
-- the whole band." Combat stats/level/faction are all overridden at
-- runtime when summoned as an active companion (level-anchored to the
-- player, role-scaled) — the values below only govern how they look and
-- behave while idle.
--
-- SCHEMA NOTE (verified on this build 2026-07-15): creature_template has NO
-- `scale`/`modelid*` columns — display + size live in creature_template_model
-- (CreatureDisplayID + DisplayScale). See mod-power-stones_broker.sql for the
-- pattern this file copies.
--
-- Display IDs picked by querying existing race-authentic NPCs in this DB:
--   Bigbilly (Orc male)     -> 4259  (Orgrimmar Grunt)
--   Tumblerr (Troll male)   -> 15574 (Darkspear Axe Thrower)
--   Onusx    (Undead male)  -> 2852  (Deathguard Abraham)
--   Denziel  (Tauren male)  -> 17332 (Tauren Warrior)
-- Mazzranache is NOT a new entry — Denziel's pet is summoned directly from
-- the existing creature_template entry 3068 (real Mulgore tallstrider).
--
-- unit_class mirrors each companion's WoW class for melee/spell table
-- resolution: 1=Warrior, 4=Rogue, 7=Shaman, 3=Hunter.
--
-- NO spawn rows here on purpose: Donnie places each with `.npc add <entry>`
-- while standing on the spot, per the locked Sanctum NPC-placement workflow.

DELETE FROM `creature_template` WHERE `entry` IN (700270, 700271, 700272, 700273);
INSERT INTO `creature_template`
    (`entry`, `name`, `subname`, `gossip_menu_id`, `minlevel`, `maxlevel`, `faction`,
     `npcflag`, `unit_class`, `type`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
    (700270, 'Bigbilly', 'The Band — Tank',   0, 1, 80, 35, 1, 1, 7, 1, 0, 'npc_sanctum_companion'),
    (700271, 'Tumblerr', 'The Band — Healer', 0, 1, 80, 35, 1, 7, 7, 1, 0, 'npc_sanctum_companion'),
    (700272, 'Onusx',    'The Band — Melee',  0, 1, 80, 35, 1, 4, 7, 1, 0, 'npc_sanctum_companion'),
    (700273, 'Denziel',  'The Band — Ranged', 0, 1, 80, 35, 1, 3, 7, 1, 0, 'npc_sanctum_companion');

DELETE FROM `creature_template_model` WHERE `CreatureID` IN (700270, 700271, 700272, 700273);
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`)
VALUES
    (700270, 0, 4259,  1, 1),
    (700271, 0, 15574, 1, 1),
    (700272, 0, 2852,  1, 1),
    (700273, 0, 17332, 1, 1);

-- Greeting text (npc_text.ID == creature entry, matched in the gossip code).
DELETE FROM `npc_text` WHERE `ID` IN (700270, 700271, 700272, 700273);
INSERT INTO `npc_text` (`ID`, `text0_0`) VALUES
    (700270, 'Hey now. Still got my back? Say the word and I''ll come tank whatever you''re about to walk into.'),
    (700271, 'Feeling nostalgic? I''ll bring the totems and keep you upright. Just say when.'),
    (700272, 'Heh. You know I''m always trying to steal aggro off the tank. Bring me along — I''ll try to behave.'),
    (700273, 'Mazzranache and I are always up for it. Just say the word and we''ll fall in behind you.');
