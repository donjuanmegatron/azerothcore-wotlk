-- mod_companion_system.sql (characters DB)
--
-- Records which of "The Band" companions a player has "called" (active,
-- following). Read on login to re-summon the band; written whenever a
-- player picks "Come with me." / "Head back to Sanctum." on a companion's
-- gossip menu. See mod-companion-system.cpp.

CREATE TABLE IF NOT EXISTS `character_companion_active` (
  `guid`             INT UNSIGNED NOT NULL COMMENT 'Owning character low-guid',
  `companion_entry`  INT UNSIGNED NOT NULL COMMENT 'creature_template entry of the active companion',
  PRIMARY KEY (`guid`, `companion_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Sanctum "The Band" — per-character active companion roster';
