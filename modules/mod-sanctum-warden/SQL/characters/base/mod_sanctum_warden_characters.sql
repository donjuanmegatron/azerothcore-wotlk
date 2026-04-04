-- mod-sanctum-warden characters SQL
-- Runs automatically against acore_characters on worldserver start.
--
-- Adds zone_chosen column to character_multiclass.
-- This tracks whether a character has completed the zone selection step
-- with the Sanctum Warden (0 = not yet, 1 = done).
--
-- Uses IF NOT EXISTS so it is safe to run multiple times
-- (e.g., worldserver restarts after the column already exists).

ALTER TABLE `character_multiclass`
    ADD COLUMN IF NOT EXISTS `zone_chosen` TINYINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT '0 = player has not chosen a starting zone yet, 1 = done';
