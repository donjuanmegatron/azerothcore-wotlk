-- Sanctum: Pet systems rework (2026-04-20)
--
-- 1. Utility guardian pets — apply 30% damage reduction.
--    These are support companions, not primary damage dealers.
--    DamageModifier 0.7 = 70% of base damage (30% reduction).
--      entry 1964  = Treant (Druid)
--      entry 19668 = Shadowfiend (Priest)
--      entry 29264 = Spirit Wolf (Shaman)
--
-- 2. All guardian creatures — faction 14 (Monster).
--    Faction 35 (Friendly to all) prevents creatures from attacking enemies.
--    Faction 14 makes them hostile to standard world mobs (also faction 14).
--    The summoner relationship prevents them from attacking their own player.
--    Note: Waterbolt/Freeze for WE are force-learned in C++ (WorldScript tick).

UPDATE creature_template SET DamageModifier = 0.7 WHERE entry IN (1964, 19668, 29264);

-- All guardian pet entries set to faction 14 so they engage world mobs.
UPDATE creature_template SET faction = 14 WHERE entry IN (
    510,   -- Mage Water Elemental
    1964,  -- Druid Treant
    19668, -- Priest Shadowfiend
    29264, -- Shaman Spirit Wolf
    26125  -- DK Risen Ghoul (guardian, summoned via Raise Dead triggered)
);
