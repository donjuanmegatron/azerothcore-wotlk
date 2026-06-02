-- mod-gear-tiers: HP/Mana Floor — patch for RequiredLevel = 0 items
-- The original 2026_04_15_hp_mana_floor.sql used RequiredLevel >= 1,
-- which skipped items with RequiredLevel = 0 (starter gear, etc.).
-- This patch applies the level 1-19 floor values to those items.

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 50,
    `stat_type10` = 0, `stat_value10` = 50
WHERE `class` = 4 AND `Quality` = 2
  AND `RequiredLevel` = 0
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 75,
    `stat_type10` = 0, `stat_value10` = 75
WHERE `class` = 4 AND `Quality` = 3
  AND `RequiredLevel` = 0
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;

UPDATE `item_template` SET
    `stat_type9` = 1, `stat_value9` = 150,
    `stat_type10` = 0, `stat_value10` = 150
WHERE `class` = 4 AND `Quality` = 4
  AND `RequiredLevel` = 0
  AND `InventoryType` IN (1,2,3,5,6,7,8,9,10,11,12,14,16,23,28)
  AND `stat_value9` = 0 AND `stat_value10` = 0
  AND `entry` < 700000;
