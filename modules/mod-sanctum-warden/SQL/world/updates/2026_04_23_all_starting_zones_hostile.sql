-- Sanctum: Restore hostile factions for ALL starting zone combat mobs.
--
-- Root cause: Starting zone mobs across all zones (except Durotar, which was never touched)
-- ended up with non-hostile factions, making them show green nameplates and unattackable.
--
-- Fix strategy: Use coordinate ranges to find every creature entry spawned in each starting
-- zone, then set faction = 14 (Monster — hostile to all players, aggro on sight) on any entry
-- that looks like a combat mob (npcflag = 0, not a critter, not flagged Civilian).
--
-- Zones covered: Elwynn/Northshire, Dun Morogh/Coldridge, Teldrassil/Shadowglen,
--               Tirisfal/Deathknell, Mulgore/Camp Narache, Azuremyst Isle/Ammen Vale,
--               Eversong Woods/Sunstrider Isle
-- Deliberately excluded: Durotar (already working), all capital cities.
--
-- Safe to run multiple times: entries that are already faction 14 are set to 14 again (no-op).

UPDATE creature_template SET faction = 14
WHERE entry IN (
    SELECT DISTINCT id1 FROM creature
    WHERE
        -- Elwynn Forest + Northshire Valley (map 0)
        (map = 0 AND position_x BETWEEN -11200 AND -8100 AND position_y BETWEEN -800 AND 1600)
        OR
        -- Dun Morogh + Coldridge Valley (map 0, west of IF)
        (map = 0 AND position_x BETWEEN -7000 AND -4600 AND position_y BETWEEN -200 AND 1200)
        OR
        -- Tirisfal Glades + Deathknell (map 0)
        (map = 0 AND position_x BETWEEN  900 AND 4600  AND position_y BETWEEN -700 AND 1900)
        OR
        -- Teldrassil + Shadowglen (map 1)
        (map = 1 AND position_x BETWEEN  8300 AND 10800 AND position_y BETWEEN  600 AND 2600)
        OR
        -- Mulgore + Camp Narache (map 1)
        (map = 1 AND position_x BETWEEN -4000 AND -1200 AND position_y BETWEEN -1400 AND  800)
        OR
        -- Azuremyst Isle + Ammen Vale (map 530)
        (map = 530 AND position_x BETWEEN -5200 AND -3100 AND position_y BETWEEN -15200 AND -12800)
        OR
        -- Eversong Woods + Sunstrider Isle (map 530)
        (map = 530 AND position_x BETWEEN  8800 AND 11600 AND position_y BETWEEN -7700 AND -5300)
)
AND entry IN (
    SELECT entry FROM (
        SELECT entry FROM creature_template
        WHERE npcflag = 0            -- no interaction flags = pure combat mob
        AND type NOT IN (8, 12)      -- exclude critters and non-combat pets
        AND flags_extra & 2 = 0      -- exclude Civilian-flagged NPCs
        AND minlevel <= 20           -- starting zone level range
        AND entry != 700200          -- never touch the Sanctum Warden
    ) AS ct_filter
);
