-- mod-pet-systems: World database tables
--
-- Pet Armory Bag item templates.
-- Each pet-capable class gets TWO bag items:
--   Small: pre-60 version
--   Large: 60+ version
-- Purchased from the Sanctum Warden (future feature).
--
-- Bag item mechanics:
--   class = 1 (ITEM_CLASS_CONTAINER), subclass = 0 (normal bag)
--   ContainerSlots = N sets the bag size
--   maxcount = 1 (one per character)
--   AllowableClass / AllowableRace = -1 (no restrictions — Sanctum handles access via NPC)
--
-- Slot counts:
--   Hunter / Warlock:                5-slot small, 8-slot large
--   DK / Druid / Shaman / Priest / Mage: 3-slot small, 5-slot large
--
-- Named bag entries (item range 700200–700213):
--   700200/700201 = Beastmaster's Pack        (Hunter)
--   700202/700203 = Grimoire of Summons       (Warlock)
--   700204/700205 = Death's Satchel           (Death Knight)
--   700206/700207 = Grove Keeper's Pouch      (Druid)
--   700208/700209 = Shaman's War Fetish       (Shaman)
--   700210/700211 = Devoted Relic             (Priest)
--   700212/700213 = Arcane Focus              (Mage)

-- Use a consistent column list for all 14 inserts.
-- AzerothCore item_template has defaults for all columns not listed here.
-- displayid 6430 = Traveler's Backpack (universal 3.3.5a icon, guaranteed in all clients).
-- Quality 4 = Epic (purple).

-- ============================================================
-- HUNTER — Beastmaster's Pack
-- ============================================================

INSERT IGNORE INTO `item_template`
    (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`,
     `Quality`, `Flags`, `FlagsExtra`, `BuyCount`, `BuyPrice`, `SellPrice`,
     `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
     `maxcount`, `stackable`, `ContainerSlots`, `bonding`, `description`)
VALUES
    (700200, 1, 0, -1, "Beastmaster's Pack", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 1, 1,
     1, 1, 5, 0,
     "A Sanctum pet bag. Items inside grant their stats to your active Hunter beast."),

    (700201, 1, 0, -1, "Beastmaster's Grand Pack", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 60, 60,
     1, 1, 8, 0,
     "A Sanctum pet bag. Items inside grant their stats to your active Hunter beast.");

-- ============================================================
-- WARLOCK — Grimoire of Summons
-- ============================================================

INSERT IGNORE INTO `item_template`
    (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`,
     `Quality`, `Flags`, `FlagsExtra`, `BuyCount`, `BuyPrice`, `SellPrice`,
     `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
     `maxcount`, `stackable`, `ContainerSlots`, `bonding`, `description`)
VALUES
    (700202, 1, 0, -1, "Grimoire of Summons", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 1, 1,
     1, 1, 5, 0,
     "A Sanctum pet bag. Items inside grant their stats to your active Warlock demon."),

    (700203, 1, 0, -1, "Grand Grimoire of Summons", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 60, 60,
     1, 1, 8, 0,
     "A Sanctum pet bag. Items inside grant their stats to your active Warlock demon.");

-- ============================================================
-- DEATH KNIGHT — Death's Satchel
-- ============================================================

INSERT IGNORE INTO `item_template`
    (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`,
     `Quality`, `Flags`, `FlagsExtra`, `BuyCount`, `BuyPrice`, `SellPrice`,
     `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
     `maxcount`, `stackable`, `ContainerSlots`, `bonding`, `description`)
VALUES
    (700204, 1, 0, -1, "Death's Satchel", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 1, 1,
     1, 1, 3, 0,
     "A Sanctum pet bag. Items inside grant their stats to your active Death Knight ghoul."),

    (700205, 1, 0, -1, "Death's Grand Satchel", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 60, 60,
     1, 1, 5, 0,
     "A Sanctum pet bag. Items inside grant their stats to your active Death Knight ghoul.");

-- ============================================================
-- DRUID — Grove Keeper's Pouch
-- ============================================================

INSERT IGNORE INTO `item_template`
    (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`,
     `Quality`, `Flags`, `FlagsExtra`, `BuyCount`, `BuyPrice`, `SellPrice`,
     `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
     `maxcount`, `stackable`, `ContainerSlots`, `bonding`, `description`)
VALUES
    (700206, 1, 0, -1, "Grove Keeper's Pouch", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 1, 1,
     1, 1, 3, 0,
     "A Sanctum pet bag. Items inside grant their stats to your summoned treants."),

    (700207, 1, 0, -1, "Grove Keeper's Satchel", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 60, 60,
     1, 1, 5, 0,
     "A Sanctum pet bag. Items inside grant their stats to your summoned treants.");

-- ============================================================
-- SHAMAN — Shaman's War Fetish
-- ============================================================

INSERT IGNORE INTO `item_template`
    (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`,
     `Quality`, `Flags`, `FlagsExtra`, `BuyCount`, `BuyPrice`, `SellPrice`,
     `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
     `maxcount`, `stackable`, `ContainerSlots`, `bonding`, `description`)
VALUES
    (700208, 1, 0, -1, "Shaman's War Fetish", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 1, 1,
     1, 1, 3, 0,
     "A Sanctum pet bag. Items inside grant their stats to your active spirit wolves."),

    (700209, 1, 0, -1, "Shaman's Grand War Fetish", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 60, 60,
     1, 1, 5, 0,
     "A Sanctum pet bag. Items inside grant their stats to your active spirit wolves.");

-- ============================================================
-- PRIEST — Devoted Relic
-- ============================================================

INSERT IGNORE INTO `item_template`
    (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`,
     `Quality`, `Flags`, `FlagsExtra`, `BuyCount`, `BuyPrice`, `SellPrice`,
     `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
     `maxcount`, `stackable`, `ContainerSlots`, `bonding`, `description`)
VALUES
    (700210, 1, 0, -1, "Devoted Relic", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 1, 1,
     1, 1, 3, 0,
     "A Sanctum pet bag. Items inside grant their stats to your shadowfiend."),

    (700211, 1, 0, -1, "Greater Devoted Relic", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 60, 60,
     1, 1, 5, 0,
     "A Sanctum pet bag. Items inside grant their stats to your shadowfiend.");

-- ============================================================
-- MAGE — Arcane Focus
-- ============================================================

INSERT IGNORE INTO `item_template`
    (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`,
     `Quality`, `Flags`, `FlagsExtra`, `BuyCount`, `BuyPrice`, `SellPrice`,
     `InventoryType`, `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
     `maxcount`, `stackable`, `ContainerSlots`, `bonding`, `description`)
VALUES
    (700212, 1, 0, -1, "Arcane Focus", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 1, 1,
     1, 1, 3, 0,
     "A Sanctum pet bag. Items inside grant their stats to your water elemental."),

    (700213, 1, 0, -1, "Greater Arcane Focus", 6430,
     4, 0, 0, 1, 0, 0,
     18, -1, -1, 60, 60,
     1, 1, 5, 0,
     "A Sanctum pet bag. Items inside grant their stats to your water elemental.");
