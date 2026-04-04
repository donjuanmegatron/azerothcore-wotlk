-- mod-dk-rework world SQL
-- Sanctum Death Knight Rework
--
-- This file runs automatically when the worldserver starts (AzerothCore SQL loader).
--
-- Two changes:
--
--  1. SPAWN POSITION
--     Updates playercreateinfo for all Death Knight race+class rows so new DKs
--     spawn at Light's Hope Chapel (map 0, Eastern Plaguelands) instead of inside
--     Acherus (map 609).  This is the same location AzerothCore uses for graduated
--     DKs when spell 50977 is present — the module grants that spell on creation.
--
--  2. TRAINER SPELL SCALING
--     All DK trainer spells currently have reqLevel values in the 55-80 range
--     (because DKs originally start at 55).  This rescales them to 1-80 so a
--     Death Knight levelling from 1 gets new abilities progressively.
--
--     Formula: new_reqLevel = ROUND(1 + (old_reqLevel - 55) * 79 / 25)
--       old 55  →  new  1
--       old 57  →  new  7
--       old 60  →  new 17
--       old 65  →  new 33
--       old 70  →  new 49
--       old 75  →  new 65
--       old 80  →  new 80
--     Rows with reqLevel = 0 (always available) are left untouched.
--
-- ============================================================
-- 1. Death Knight starting position → Light's Hope Chapel
-- ============================================================
--
-- Light's Hope Chapel coordinates (Eastern Kingdoms, map 0):
--   X =  2352.0,  Y = -5709.0,  Z = 154.5,  O = 3.14 (facing west)
-- zone = 139 (Eastern Plaguelands)
--
-- class 6 = CLASS_DEATH_KNIGHT.  All ten DK-eligible races are updated.

-- Spawn DKs in Dalaran — the Sanctum hub, neutral to all factions.
UPDATE `playercreateinfo`
SET
    `map`         = 571,
    `zone`        = 4395,
    `position_x`  = 5804.15,
    `position_y`  = 624.77,
    `position_z`  = 648.09,
    `orientation` = 1.57
WHERE `class` = 6;

-- ============================================================
-- 2. Rescale Death Knight trainer spell requirements: 55-80 → 1-80
--
-- The modern AzerothCore trainer schema uses:
--   trainer        — Id, Type (0=class), Requirement (classId)
--   trainer_spell  — TrainerId, SpellId, ReqLevel
-- ============================================================

UPDATE `trainer_spell` ts
INNER JOIN `trainer` t ON t.`Id` = ts.`TrainerId`
SET ts.`ReqLevel` = LEAST(80, GREATEST(1, ROUND(1 + (ts.`ReqLevel` - 55.0) * 79.0 / 25.0)))
WHERE t.`Requirement` = 6
  AND t.`Type` = 0
  AND ts.`SpellId` > 0
  AND ts.`ReqLevel` >= 55;
