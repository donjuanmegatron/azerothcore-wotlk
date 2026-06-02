-- Sanctum: Unlock Classic instance key-locked doors (2026-04-20)
--
-- In AzerothCore 3.3.5a, Blizzard's WotLK patch 3.0 changes already removed most
-- Classic dungeon key requirements. The following are confirmed ALREADY OPEN:
--   Dire Maul North/West   (Crescent Key 18249 — not enforced)
--   Stratholme back gate   (Key to the City 12382 — not enforced)
--   UBRS                   (Seal of Ascension 12344 — removed in patch 3.0)
--   Blackwing Lair         (no item barrier, progression-based only)
--   Onyxia's Lair          (no entry barrier)
--   Molten Core            (no entry barrier)
--
-- Only TWO Classic locks are still active in the AC 3.3.5a database:
--
-- 1. BRD — Shadowforge Lock (inside instance)
--      Gameobject entries 170559, 170560 (Shadowforge Gates)
--      Lock ID 680 — requires Shadowforge Key (item 11286)
--
-- 2. Scholomance — front door
--      Gameobject entry 174626 (Scholomance Door)
--      Lock ID 1159 — requires Skeleton Key (item 13704)
--
-- Fix: remove the lock from the gameobject_template AND insert a zero-entry into
-- lock_dbc to override the DBC data. Both together guarantee the doors open freely.

-- ---------------------------------------------------------------
-- Step 1: Remove lock reference from the gameobject templates
-- A gameobject with lock=0 is always interactable with no key check.
-- ---------------------------------------------------------------

UPDATE gameobject_template SET lock = 0 WHERE entry IN (
    170559,  -- BRD Shadowforge Gate (inner)
    170560,  -- BRD Shadowforge Gate (outer)
    174626   -- Scholomance front door
);

-- ---------------------------------------------------------------
-- Step 2: Override Lock.dbc entries in the world database
-- AzerothCore reads lock data from client DBC but respects lock_dbc overrides.
-- Inserting all-zero rows means "no requirement" — effectively unlocked.
-- ---------------------------------------------------------------

DELETE FROM lock_dbc WHERE ID IN (680, 1159);

INSERT INTO lock_dbc
    (ID,
     type1, type2, type3, type4, type5, type6, type7, type8,
     properties1, properties2, properties3, properties4,
     properties5, properties6, properties7, properties8,
     requiredminingskill, requiredlockskill)
VALUES
(680,  0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0),  -- BRD Shadowforge Lock
(1159, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0);  -- Scholomance door
