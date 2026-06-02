-- mod-pet-systems: Character database tables
--
-- character_pet_bag_cache
--   Caches which bag slot each player has their pet bag in, and the
--   total stat bonus currently applied to their pet from that bag.
--   Updated whenever the bag contents change (TODO: hook ItemEquip/Unequip).
--
-- character_pet_stats_applied
--   Records the last stat bonuses applied to each player's pet so they
--   can be removed (reversed) before re-applying on level-up or bag change.
--   Without tracking the previously-applied values, stats would stack
--   each login instead of replacing.

CREATE TABLE IF NOT EXISTS `character_pet_bag_cache` (
    `guid`         INT UNSIGNED NOT NULL,
    `class_id`     TINYINT UNSIGNED NOT NULL,   -- which pet class this bag is for (1-11)
    `bag_entry`    INT UNSIGNED NOT NULL DEFAULT 0, -- item_template entry of the equipped pet bag
    `bag_slot`     TINYINT UNSIGNED NOT NULL DEFAULT 0, -- which bag slot it occupies (19-22)
    `last_updated` INT UNSIGNED NOT NULL DEFAULT 0, -- unix timestamp of last cache rebuild
    PRIMARY KEY (`guid`, `class_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Sanctum pet bag slot cache per character per pet class';

CREATE TABLE IF NOT EXISTS `character_pet_stats_applied` (
    `guid`          INT UNSIGNED NOT NULL,
    `stat_strength` FLOAT NOT NULL DEFAULT 0,
    `stat_agility`  FLOAT NOT NULL DEFAULT 0,
    `stat_stamina`  FLOAT NOT NULL DEFAULT 0,
    `stat_intellect` FLOAT NOT NULL DEFAULT 0,
    `stat_spirit`   FLOAT NOT NULL DEFAULT 0,
    `stat_sp`       FLOAT NOT NULL DEFAULT 0,  -- spell power (TODO)
    `stat_ap`       FLOAT NOT NULL DEFAULT 0,  -- attack power (TODO)
    `source`        TINYINT UNSIGNED NOT NULL DEFAULT 0, -- 0=owner%, 1=bag items, 2=both
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Last pet stat bonus snapshot — used to reverse bonuses before re-applying';
