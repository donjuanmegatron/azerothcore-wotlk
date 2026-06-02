-- mod-xp-system: first-encounter kill tracking per character.
-- Applied to acore_characters DB.
CREATE TABLE IF NOT EXISTS `character_xp_first_kill` (
  `guid`           INT UNSIGNED NOT NULL,
  `creature_entry` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`guid`, `creature_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
