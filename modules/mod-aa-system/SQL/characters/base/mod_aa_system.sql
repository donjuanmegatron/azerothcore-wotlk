-- mod-aa-system: Sanctum Alternate Advancement
-- Two tables per character:
--   character_aa_points   — XP pool, points earned, points spent
--   character_aa_purchased — which AAs the character has bought and at what rank

CREATE TABLE IF NOT EXISTS `character_aa_points` (
    `guid`           INT UNSIGNED NOT NULL,
    `aa_xp`          BIGINT UNSIGNED NOT NULL DEFAULT 0,   -- raw accumulated AA XP
    `points_earned`  INT UNSIGNED NOT NULL DEFAULT 0,       -- total points ever earned (aa_xp / COST_PER_POINT)
    `points_spent`   INT UNSIGNED NOT NULL DEFAULT 0,       -- total points spent on AAs
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Sanctum AA point pool per character';

CREATE TABLE IF NOT EXISTS `character_aa_purchased` (
    `guid`    INT UNSIGNED NOT NULL,
    `aa_id`   INT UNSIGNED NOT NULL,          -- AA identifier (see enum in mod-aa-system.cpp)
    `aa_rank` TINYINT UNSIGNED NOT NULL DEFAULT 1,   -- current rank 1-5
    PRIMARY KEY (`guid`, `aa_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Sanctum AA purchased abilities per character';
