-- Sanctum Warden spawn at Camp Narache (Red Cloud Mesa) — Tauren starting zone.
-- map=1, Mulgore. Placed at center of camp near the other NPCs.
INSERT INTO creature (id1, id2, id3, map, zoneId, areaId, spawnMask, phaseMask, equipment_id,
    position_x, position_y, position_z, orientation,
    spawntimesecs, wander_distance, currentwaypoint, curhealth, curmana,
    MovementType, npcflag, unit_flags, dynamicflags, ScriptName, VerifiedBuild, CreateObject, Comment)
VALUES (700200, 0, 0, 1, 0, 0, 1, 1, 0,
    -2942.0, -237.0, 53.8, 4.71,
    300, 0, 0, 1, 0,
    0, 0, 0, 0, '', NULL, 0, 'Sanctum Warden - Camp Narache');
