-- mod-sanctum-warden: Starter gear item templates
-- Granted by the Sanctum Warden when a new character selects their starting zone.
-- Each of the player's 3 chosen classes contributes 1 armor piece + 1 weapon (or 2 for DK).
--
-- Item entry range: 700300–700320
--   700300/700301 = Warrior   (leather vest + 2H sword)
--   700302/700303 = Paladin   (leather vest + 1H mace w/ Spell Power)
--   700304/700305 = Hunter    (leather vest + shortbow)
--   700306/700307 = Rogue     (leather vest + dagger, qty 2 granted)
--   700308/700309 = Priest    (cloth robe + staff)
--   700310/700311 = DK        (leather vest + 2H sword)
--   700312/700313 = Shaman    (leather vest + 2H mace)
--   700314/700315 = Mage      (cloth robe + staff)
--   700316/700317 = Warlock   (cloth robe + staff)
--   700318/700319 = Druid     (leather vest + staff)
--   700320        = DK 1H sword (granted x2 alongside 700311 — both weapon sets)
--
-- All items: green quality (2), item level 5, required level 0, no class restriction.
-- Stats are modest (level 1 green) — replaced quickly by quest rewards.
-- Weapons have real DPS values; armor provides primary stat only.
--
-- ITEM_MOD values used: 3=AGI 4=STA 5=INT 6=SPI 7=STR 13=SPELL_POWER
-- InventoryType:  5=CHEST  13=ONE_HAND  15=RANGED  17=TWO_HAND
-- Weapon subclass: 1=AXE 4=MACE 5=MACE2H 7=SWORD 8=SWORD2H 10=STAFF 15=DAGGER
-- Armor subclass:  1=CLOTH  2=LEATHER
-- Display IDs from existing WoW 3.3.5a models verified in DB.

-- ============================================================
-- WARRIOR  (700300 chest, 700301 2H sword)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Initiate's Mail Vest: STR+5, STA+3
    (700300,4,3,-1,'Initiate''s Mail Vest',2222,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     7,5,4,3,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Apprentice's Greatsword: 2H sword, STR+3
    (700301,2,8,-1,'Apprentice''s Greatsword',22093,
     2,0,0,1,0,0,
     17,-1,-1,5,0,
     1,1,
     7,3,0,0,
     10,17,0,3300,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- PALADIN  (700302 chest, 700303 1H mace w/ Spell Power)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,`stat_type3`,`stat_value3`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Protector's Mail Vest: STR+4, STA+3
    (700302,4,3,-1,'Protector''s Mail Vest',2222,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     7,4,4,3,0,0,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Apprentice's Mace: 1H mace, STR+3, INT+3, Spell Power+5
    (700303,2,4,-1,'Apprentice''s Mace',22118,
     2,0,0,1,0,0,
     13,-1,-1,5,0,
     1,1,
     7,3,5,3,13,5,
     5,9,0,2600,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- HUNTER  (700304 chest, 700305 shortbow)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Scout's Leather Vest: AGI+5, STA+3
    (700304,4,2,-1,'Scout''s Leather Vest',2106,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     3,5,4,3,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Rough Shortbow: ranged bow, AGI+3
    (700305,2,2,-1,'Rough Shortbow',8106,
     2,0,0,1,0,0,
     15,-1,-1,5,0,
     1,1,
     3,3,0,0,
     10,14,0,2900,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- ROGUE  (700306 chest, 700307 dagger — granted x2)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Cutthroat's Leather Vest: AGI+5, STA+3
    (700306,4,2,-1,'Cutthroat''s Leather Vest',2106,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     3,5,4,3,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Worn Dagger: dagger, AGI+3 (granted as qty 2 for dual-wield)
    (700307,2,15,-1,'Worn Dagger',6442,
     2,0,0,1,0,0,
     13,-1,-1,5,0,
     1,1,
     3,3,0,0,
     3,5,0,1700,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- PRIEST  (700308 cloth robe, 700309 staff)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Acolyte's Robe: INT+4, SPI+4
    (700308,4,1,-1,'Acolyte''s Robe',16579,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     5,4,6,4,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Acolyte's Staff: INT+2, SPI+2
    (700309,2,10,-1,'Acolyte''s Staff',20432,
     2,0,0,1,0,0,
     17,-1,-1,5,0,
     1,1,
     5,2,6,2,
     7,11,0,3200,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- DEATH KNIGHT  (700310 leather vest, 700311 2H sword, 700320 1H sword x2)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Runeguard's Mail Vest: STR+5, STA+4
    (700310,4,3,-1,'Runeguard''s Mail Vest',2222,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     7,5,4,4,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Runeblade of the Initiate: 2H sword, STR+4
    (700311,2,8,-1,'Runeblade of the Initiate',22093,
     2,0,0,1,0,0,
     17,-1,-1,5,0,
     1,1,
     7,4,0,0,
     10,17,0,3300,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Initiate's Blade: 1H sword, STR+3 (granted x2 for dual-wield Frost DK)
    (700320,2,7,-1,'Initiate''s Blade',26577,
     2,0,0,1,0,0,
     13,-1,-1,5,0,
     1,1,
     7,3,0,0,
     5,9,0,2600,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- SHAMAN  (700312 leather vest, 700313 2H mace)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,`stat_type3`,`stat_value3`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Shaman's Leather Vest: AGI+3, INT+3, STA+2
    (700312,4,2,-1,'Shaman''s Leather Vest',2106,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     3,3,5,3,4,2,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Earthen Mace: 2H mace, AGI+2, INT+2
    (700313,2,5,-1,'Earthen Mace',8690,
     2,0,0,1,0,0,
     17,-1,-1,5,0,
     1,1,
     3,2,5,2,0,0,
     10,17,0,3300,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- MAGE  (700314 cloth robe, 700315 staff)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Apprentice's Robe: INT+5, SPI+3
    (700314,4,1,-1,'Apprentice''s Robe',16579,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     5,5,6,3,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Apprentice's Staff: INT+3, SPI+2
    (700315,2,10,-1,'Apprentice''s Staff',20432,
     2,0,0,1,0,0,
     17,-1,-1,5,0,
     1,1,
     5,3,6,2,
     7,11,0,3200,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- WARLOCK  (700316 cloth robe, 700317 staff)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Fel-Touched Robe: INT+4, SPI+3
    (700316,4,1,-1,'Fel-Touched Robe',16579,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     5,4,6,3,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Warlock''s Staff: INT+3, SPI+2
    (700317,2,10,-1,'Warlock''s Staff',20432,
     2,0,0,1,0,0,
     17,-1,-1,5,0,
     1,1,
     5,3,6,2,
     7,11,0,3200,
     1,'Starter gear granted by the Sanctum Warden.');

-- ============================================================
-- DRUID  (700318 leather vest, 700319 staff)
-- ============================================================
INSERT IGNORE INTO `item_template`
    (`entry`,`class`,`subclass`,`SoundOverrideSubclass`,`name`,`displayid`,
     `Quality`,`Flags`,`FlagsExtra`,`BuyCount`,`BuyPrice`,`SellPrice`,
     `InventoryType`,`AllowableClass`,`AllowableRace`,`ItemLevel`,`RequiredLevel`,
     `maxcount`,`stackable`,
     `stat_type1`,`stat_value1`,`stat_type2`,`stat_value2`,`stat_type3`,`stat_value3`,
     `dmg_min1`,`dmg_max1`,`dmg_type1`,`delay`,
     `bonding`,`description`)
VALUES
    -- Druid's Leather Vest: AGI+3, INT+3, SPI+2
    (700318,4,2,-1,'Druid''s Leather Vest',2106,
     2,0,0,1,0,0,
     5,-1,-1,5,0,
     1,1,
     3,3,5,3,6,2,
     0,0,0,0,
     1,'Starter gear granted by the Sanctum Warden.'),
    -- Ironwood Staff: AGI+2, INT+2, SPI+2
    (700319,2,10,-1,'Ironwood Staff',20432,
     2,0,0,1,0,0,
     17,-1,-1,5,0,
     1,1,
     3,2,5,2,6,2,
     7,11,0,3200,
     1,'Starter gear granted by the Sanctum Warden.');
