-- Sanctum: Restore hostile factions for Northshire / Elwynn Forest combat mobs.
--
-- Root cause: An earlier broad faction=35 update (or the base SQL targeting certain
-- faction IDs) set all these combat mobs to faction 35 (Friendly to all). This made
-- them show a green nameplate and be unattackable by the player.
--
-- Fix: Set them to faction 14 (Monster — hostile to all player factions, aggro on sight).
-- Faction 14 is the standard WoW aggressive-beast/monster faction. It is NOT in the
-- base SQL faction list, so server restarts will not revert this.
--
-- Scope: Northshire Valley / Elwynn Forest combat mobs only.
-- All entries confirmed to spawn exclusively on map 0 (Eastern Kingdoms).

UPDATE creature_template SET faction = 14 WHERE entry IN (
    -- Wolves
    299,   -- Diseased Young Wolf
    69,    -- Diseased Timber Wolf
    525,   -- Mangy Wolf
    1922,  -- Gray Forest Wolf

    -- Kobolds
    6,     -- Kobold Vermin
    80,    -- Kobold Laborer
    257,   -- Kobold Worker
    476,   -- Kobold Geomancer

    -- Defias
    38,    -- Defias Thug
    94,    -- Defias Cutpurse
    116,   -- Defias Bandit
    474,   -- Defias Rogue Wizard

    -- Wildlife
    30,    -- Forest Spider
    43,    -- Mine Spider   (was 22 — already worked but consolidating)
    113,   -- Stonetusk Boar
    822,   -- Young Forest Bear

    -- Murlocs (Elwynn riverside)
    285,   -- Murloc
    735    -- Murloc Streamrunner
);

-- Dun Morogh starting zone wolves (same problem)
UPDATE creature_template SET faction = 14 WHERE entry IN (
    705,   -- Ragged Young Wolf   (was 32 — neutral, now properly hostile)
    704    -- Ragged Timber Wolf  (was 32)
);
