-- mod-gear-tiers: Armory Slot system
-- Tracks which item each character has designated as their Armory item,
-- how much Gear XP it has accumulated, and what tier it is.
--
-- Tiers:
--   0 = Normal     (base stats)
--   1 = Enchanted  (1.4x — reached at 10,000 GXP)
--   2 = Legendary  (2.0x — reached at 60,000 total GXP)

CREATE TABLE IF NOT EXISTS `character_armory_slot` (
    `guid`       INT UNSIGNED    NOT NULL,
    `item_guid`  INT UNSIGNED    NOT NULL DEFAULT 0,
    `item_entry` INT UNSIGNED    NOT NULL DEFAULT 0,
    `gear_xp`    INT UNSIGNED    NOT NULL DEFAULT 0,
    `tier`       TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Sanctum Armory Slot designation per character';
