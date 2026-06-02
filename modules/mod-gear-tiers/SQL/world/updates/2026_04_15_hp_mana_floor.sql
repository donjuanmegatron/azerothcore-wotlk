-- mod-gear-tiers: HP/Mana Floor System
-- Applies flat HP and mana minimums to all world armor (green/blue/purple).
-- Uses stat slots 9 (HP, type=1) and 10 (mana, type=0).
-- Stacks on top of the existing % stat bonus system in mod-gear-tiers.
-- Only targets items with empty stat slots 9 and 10 (stat_value = 0).
--
-- Quality values: 2=Uncommon (green), 3=Rare (blue), 4=Epic (purple)
-- Rare multiplier: 1.5x green floor. Epic multiplier: 3x green floor.
--
-- Level floors (green / rare / epic):
--   1-19:  50 / 75 / 150
--   20-29: 75 / 112 / 225
--   30-39: 100 / 150 / 300
--   40-49: 150 / 225 / 450
--   50-59: 200 / 300 / 600
--   60-69: 300 / 450 / 900
--   70-79: 400 / 600 / 1200
--   80:    500 / 750 / 1500
--
-- class=4 = ITEM_CLASS_ARMOR
-- entry < 700000 = skip all custom Sanctum items

-- ============================================================
-- LEVEL 1-19
-- ============================================================

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 50,
    `stat_type10` = 0, `stat_value10` = 50
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` >= 1 AND `RequiredLevel` <= 19
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 75,
    `stat_type10` = 0, `stat_value10` = 75
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` >= 1 AND `RequiredLevel` <= 19
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 150,
    `stat_type10` = 0, `stat_value10` = 150
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` >= 1 AND `RequiredLevel` <= 19
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

-- ============================================================
-- LEVEL 20-29
-- ============================================================

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 75,
    `stat_type10` = 0, `stat_value10` = 75
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` >= 20 AND `RequiredLevel` <= 29
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 112,
    `stat_type10` = 0, `stat_value10` = 112
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` >= 20 AND `RequiredLevel` <= 29
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 225,
    `stat_type10` = 0, `stat_value10` = 225
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` >= 20 AND `RequiredLevel` <= 29
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

-- ============================================================
-- LEVEL 30-39
-- ============================================================

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 100,
    `stat_type10` = 0, `stat_value10` = 100
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` >= 30 AND `RequiredLevel` <= 39
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 150,
    `stat_type10` = 0, `stat_value10` = 150
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` >= 30 AND `RequiredLevel` <= 39
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 300,
    `stat_type10` = 0, `stat_value10` = 300
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` >= 30 AND `RequiredLevel` <= 39
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

-- ============================================================
-- LEVEL 40-49
-- ============================================================

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 150,
    `stat_type10` = 0, `stat_value10` = 150
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` >= 40 AND `RequiredLevel` <= 49
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 225,
    `stat_type10` = 0, `stat_value10` = 225
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` >= 40 AND `RequiredLevel` <= 49
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 450,
    `stat_type10` = 0, `stat_value10` = 450
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` >= 40 AND `RequiredLevel` <= 49
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

-- ============================================================
-- LEVEL 50-59
-- ============================================================

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 200,
    `stat_type10` = 0, `stat_value10` = 200
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` >= 50 AND `RequiredLevel` <= 59
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 300,
    `stat_type10` = 0, `stat_value10` = 300
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` >= 50 AND `RequiredLevel` <= 59
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 600,
    `stat_type10` = 0, `stat_value10` = 600
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` >= 50 AND `RequiredLevel` <= 59
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

-- ============================================================
-- LEVEL 60-69
-- ============================================================

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 300,
    `stat_type10` = 0, `stat_value10` = 300
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` >= 60 AND `RequiredLevel` <= 69
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 450,
    `stat_type10` = 0, `stat_value10` = 450
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` >= 60 AND `RequiredLevel` <= 69
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 900,
    `stat_type10` = 0, `stat_value10` = 900
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` >= 60 AND `RequiredLevel` <= 69
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

-- ============================================================
-- LEVEL 70-79
-- ============================================================

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 400,
    `stat_type10` = 0, `stat_value10` = 400
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` >= 70 AND `RequiredLevel` <= 79
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 600,
    `stat_type10` = 0, `stat_value10` = 600
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` >= 70 AND `RequiredLevel` <= 79
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 1200,
    `stat_type10` = 0, `stat_value10` = 1200
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` >= 70 AND `RequiredLevel` <= 79
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

-- ============================================================
-- LEVEL 80
-- ============================================================

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 500,
    `stat_type10` = 0, `stat_value10` = 500
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` = 80
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 750,
    `stat_type10` = 0, `stat_value10` = 750
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` = 80
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 1500,
    `stat_type10` = 0, `stat_value10` = 1500
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` = 80
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;
