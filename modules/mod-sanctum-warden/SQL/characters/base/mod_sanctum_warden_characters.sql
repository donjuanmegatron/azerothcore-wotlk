-- mod-sanctum-warden characters SQL
-- Runs automatically against acore_characters on worldserver start.
--
-- Adds zone_chosen column to character_multiclass.
-- This tracks whether a character has completed the zone selection step
-- with the Sanctum Warden (0 = not yet, 1 = done).
--
-- Uses a prepared statement pattern so it is safe to run multiple times
-- (will skip the ALTER if the column already exists).

SET @col_exists = (
    SELECT COUNT(*)
    FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME   = 'character_multiclass'
      AND COLUMN_NAME  = 'zone_chosen'
);

SET @sql = IF(
    @col_exists = 0,
    'ALTER TABLE `character_multiclass` ADD COLUMN `zone_chosen` TINYINT UNSIGNED NOT NULL DEFAULT 0',
    'SELECT ''zone_chosen column already exists — skipping'''
);

PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
