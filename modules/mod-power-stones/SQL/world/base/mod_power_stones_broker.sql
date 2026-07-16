-- mod-power-stones PHASE 4: Acquisition NPC (gossip shop front)
-- ---------------------------------------------------------------------------
-- "Lapidary Voss, Power Stone Broker" — creature_template entry 700260,
-- script npc_power_stone_broker (mod-power-stones.cpp). A pure SHOP: buy new
-- stones, rank/tier up owned stones, view collection — all spending Conquest
-- Shards. Socketing stays on the .stone commands + the Phase 6 Lua panel.
--
-- SCHEMA NOTE (verified against this AC build 2026-07-15): creature_template on
-- this core has NO `scale` and NO `modelid*` columns. Display model + size live
-- in creature_template_model (CreatureDisplayID + DisplayScale). Do NOT re-add a
-- `scale` column to the INSERT below — it will fail with "Unknown column".
--
-- Reskin/rename later: edit `name`/`subname` + CreatureDisplayID (15504 =
-- Gelanthis, a Blood Elf jewelcrafter — a fitting gem-broker look) — no C++
-- change needed.

DELETE FROM `creature_template` WHERE `entry` = 700260;
INSERT INTO `creature_template`
    (`entry`, `name`, `subname`, `gossip_menu_id`, `minlevel`, `maxlevel`, `faction`,
     `npcflag`, `unit_class`, `type`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
    (700260, 'Lapidary Voss', 'Power Stone Broker', 0, 80, 80, 35,
     1, 1, 7, 1, 2, 'npc_power_stone_broker');

DELETE FROM `creature_template_model` WHERE `CreatureID` = 700260;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`)
VALUES (700260, 0, 15504, 1, 1);

-- Greeting text at the top of the gossip window.
DELETE FROM `npc_text` WHERE `ID` = 700260;
INSERT INTO `npc_text` (`ID`, `text0_0`) VALUES
    (700260, 'Ah, a collector of power. I am Voss — I cut and trade the Conquest Stones. Bring me shards and I will sell you stones, or hone the ones you already carry to a keener edge. What will it be?');

-- NO spawn row here on purpose: place Voss yourself with `.npc add 700260`
-- while standing on the exact spot (a hand-typed .gps Z can read ~30yd high and
-- float the NPC), per the Sanctum NPC-placement workflow.
