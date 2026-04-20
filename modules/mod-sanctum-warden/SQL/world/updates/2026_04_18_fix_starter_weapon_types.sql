-- Fix starter weapon item_template data.
-- INSERT IGNORE means prior bad inserts are never corrected.
-- This UPDATE forces the correct class/subclass/InventoryType values
-- for all melee starter weapons so warrior (and other class) abilities
-- recognise them correctly.
--
-- class=2      = Weapon
-- InventoryType: 13=ONE_HAND  17=TWO_HAND
-- Subclass:      4=Mace1H  5=Mace2H  7=Sword1H  8=Sword2H  10=Staff  15=Dagger

-- Warrior: Apprentice's Greatsword (2H sword)
UPDATE item_template SET
    class=2, subclass=8, InventoryType=17,
    dmg_min1=10, dmg_max1=17, dmg_type1=0, delay=3300
WHERE entry=700301;

-- Paladin: Apprentice's Mace (1H mace)
UPDATE item_template SET
    class=2, subclass=4, InventoryType=13,
    dmg_min1=5, dmg_max1=9, dmg_type1=0, delay=2600
WHERE entry=700303;

-- Rogue: Worn Dagger (1H dagger x2)
UPDATE item_template SET
    class=2, subclass=15, InventoryType=13,
    dmg_min1=3, dmg_max1=5, dmg_type1=0, delay=1700
WHERE entry=700307;

-- Priest: Acolyte's Staff (2H staff)
UPDATE item_template SET
    class=2, subclass=10, InventoryType=17,
    dmg_min1=7, dmg_max1=11, dmg_type1=0, delay=3200
WHERE entry=700309;

-- Death Knight: Runeblade of the Initiate (2H sword)
UPDATE item_template SET
    class=2, subclass=8, InventoryType=17,
    dmg_min1=10, dmg_max1=17, dmg_type1=0, delay=3300
WHERE entry=700311;

-- Death Knight: Initiate's Blade (1H sword x2)
UPDATE item_template SET
    class=2, subclass=7, InventoryType=13,
    dmg_min1=5, dmg_max1=9, dmg_type1=0, delay=2600
WHERE entry=700320;

-- Shaman: Earthen Mace (2H mace)
UPDATE item_template SET
    class=2, subclass=5, InventoryType=17,
    dmg_min1=10, dmg_max1=17, dmg_type1=0, delay=3300
WHERE entry=700313;

-- Mage: Apprentice's Staff (2H staff)
UPDATE item_template SET
    class=2, subclass=10, InventoryType=17,
    dmg_min1=7, dmg_max1=11, dmg_type1=0, delay=3200
WHERE entry=700315;

-- Warlock: Warlock's Staff (2H staff)
UPDATE item_template SET
    class=2, subclass=10, InventoryType=17,
    dmg_min1=7, dmg_max1=11, dmg_type1=0, delay=3200
WHERE entry=700317;

-- Druid: Ironwood Staff (2H staff)
UPDATE item_template SET
    class=2, subclass=10, InventoryType=17,
    dmg_min1=7, dmg_max1=11, dmg_type1=0, delay=3200
WHERE entry=700319;
