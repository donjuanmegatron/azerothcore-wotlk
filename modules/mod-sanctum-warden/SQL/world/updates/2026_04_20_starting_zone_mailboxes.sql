-- Sanctum: Mailboxes at all 8 Warden starting zone teleport destinations (2026-04-20)
--
-- New characters receive their starter gear kit via mail from "The Architect".
-- Without a nearby mailbox they cannot retrieve it. This places one mailbox
-- at (or within a few steps of) each Warden teleport coordinate.
--
-- Gameobject entry 32349 = standard Mailbox (works for all factions).
-- GUIDs 700100–700107 — custom Sanctum range, no conflict with base DB.
--
-- Zones and their Warden teleport coordinates:
--   Elwynn Forest    map 0   (-8940,  -131,   83.53)
--   Dun Morogh       map 0   (-6235,   333,  382.76)
--   Teldrassil       map 1   (10313,   834, 1326.41)
--   Tirisfal Glades  map 0   ( 1680,  1680,  121.67)
--   Durotar          map 1   ( -614, -4249,   38.72)
--   Mulgore          map 1   (-2915,  -256,   52.99)
--   Azuremyst Isle   map 530 (-3959,-13929,  100.61)
--   Eversong Woods   map 530 (10351, -6355,   33.40)
--
-- NOTE: These are placed at the teleport coordinates + a small X offset so the
-- mailbox is right next to the spawn point. If a mailbox appears floating or
-- buried in terrain, adjust in-game:
--   .gps                    (get nearby coords)
--   UPDATE gameobject SET position_x=?, position_y=?, position_z=? WHERE guid=700100;
--   .reload gameobject      (no restart needed)
--
-- Existing base-DB mailboxes in these zones are left alone. The new ones are
-- a convenience spawn at the exact landing spot.

DELETE FROM gameobject WHERE guid IN (700100, 700101, 700102, 700103, 700104, 700105, 700106, 700107);

INSERT INTO gameobject
    (guid,   id,  map, spawnMask, phaseMask, position_x,  position_y,   position_z, orientation, rotation0, rotation1, rotation2, rotation3, spawntimesecs, animprogress, state)
VALUES
-- Elwynn Forest (Human)
(700100, 32349,   0,    1,       1,  -8937.00,    -131.00,       83.53,        0.00,       0.00,     0.00,     0.00,     1.00,           120,          255,     1),
-- Dun Morogh (Dwarf / Gnome)
(700101, 32349,   0,    1,       1,  -6232.00,     333.00,      382.76,        0.00,       0.00,     0.00,     0.00,     1.00,           120,          255,     1),
-- Teldrassil (Night Elf)
(700102, 32349,   1,    1,       1,  10316.00,     834.00,     1326.41,        0.00,       0.00,     0.00,     0.00,     1.00,           120,          255,     1),
-- Tirisfal Glades (Undead)
(700103, 32349,   0,    1,       1,   1683.00,    1680.00,      121.67,        0.00,       0.00,     0.00,     0.00,     1.00,           120,          255,     1),
-- Durotar (Orc / Troll)
(700104, 32349,   1,    1,       1,   -611.00,   -4249.00,       38.72,        0.00,       0.00,     0.00,     0.00,     1.00,           120,          255,     1),
-- Mulgore (Tauren)
(700105, 32349,   1,    1,       1,  -2912.00,    -256.00,       52.99,        0.00,       0.00,     0.00,     0.00,     1.00,           120,          255,     1),
-- Azuremyst Isle (Draenei)
(700106, 32349, 530,    1,       1,  -3956.00,  -13929.00,      100.61,        0.00,       0.00,     0.00,     0.00,     1.00,           120,          255,     1),
-- Eversong Woods (Blood Elf)
(700107, 32349, 530,    1,       1,  10354.00,   -6355.00,       33.40,        0.00,       0.00,     0.00,     0.00,     1.00,           120,          255,     1);
