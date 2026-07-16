-- mod_power_stones.sql
--
-- Sanctum Power Stones — Phase 2: catalog (hardcoded in C++) + player collection.
--
-- character_power_stones: one row per OWNED stone instance. Stat values are NOT
-- stored here — they're derived at read time from (stone_type, tier, rank)
-- against the hardcoded catalog in mod-power-stones.cpp, so tuning the catalog
-- never requires a data migration.
--
-- Socketing a stone into a gear slot is a SEPARATE table planned for Phase 3
-- (e.g. character_power_stone_sockets) — intentionally NOT created here.

CREATE TABLE IF NOT EXISTS `character_power_stones` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `guid`       INT UNSIGNED    NOT NULL COMMENT 'Owning character low-guid',
  `stone_type` TINYINT UNSIGNED NOT NULL COMMENT '1=Crimson 2=Obsidian 3=Jade 4=Iron 5=Amber',
  `tier`       TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1-5',
  `rank`       TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1-3',
  PRIMARY KEY (`id`),
  KEY `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Sanctum Power Stones — per-character owned stone instances (Phase 2)';
