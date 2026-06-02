-- Springpaw Cub (15366) and Springpaw Lynx (15372) at Sunstrider Isle were excluded
-- from the bulk hostile SQL because they had flags_extra = 2 (Civilian flag).
-- Clearing the Civilian flag and setting faction = 14 so they are hostile like all
-- other Eversong Woods starting zone mobs.
UPDATE creature_template SET faction = 14, flags_extra = 0 WHERE entry IN (15366, 15372);
