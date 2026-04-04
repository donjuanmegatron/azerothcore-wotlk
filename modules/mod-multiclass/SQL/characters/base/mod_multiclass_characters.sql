-- mod-multiclass characters database SQL
-- Runs automatically against acore_characters on worldserver start.
-- Creates the table that stores each character's three chosen classes.

-- ============================================================
-- character_multiclass
--
-- guid           = character GUID (matches characters.guid)
-- class1         = primary class (set from WoW character creation, never changes)
-- class2         = second chosen class (0 until player picks one at the NPC)
-- class3         = third chosen class (0 until player picks one at the NPC)
-- selection_step = 0: needs class2 | 1: needs class3 | 2: selection complete
-- ============================================================

CREATE TABLE IF NOT EXISTS `character_multiclass` (
    `guid`           INT UNSIGNED    NOT NULL,
    `class1`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `class2`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `class3`         TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `selection_step` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Sanctum multiclass — three classes per character';
