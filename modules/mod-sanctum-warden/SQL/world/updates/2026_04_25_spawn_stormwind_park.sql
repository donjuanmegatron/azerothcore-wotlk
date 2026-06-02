-- New character spawn point: The Park, Stormwind City (2026-04-25)
--
-- Overrides mod-dk-rework's Dalaran spawn for DKs and the default
-- race-specific spawns for all other classes.
-- Runs as an update file so it persists across restarts and always
-- executes after the base files that set Dalaran coordinates.

UPDATE `playercreateinfo`
SET
    `map`         = 0,
    `zone`        = 1519,
    `position_x`  = -8435.026,
    `position_y`  = 402.612,
    `position_z`  = 120.886,
    `orientation` = 5.06;
