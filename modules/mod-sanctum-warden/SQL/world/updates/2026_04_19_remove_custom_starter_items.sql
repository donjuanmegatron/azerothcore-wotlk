-- Sanctum: Remove all custom starter gear entries (700300–700320).
-- These were replaced by real game items in mod-sanctum-warden.cpp.
-- The KITS table now references only items that already exist in
-- the WotLK client — no custom item_template rows needed.
--
-- Removed items:
--   700300 Initiate's Mail Vest       (Warrior chest)
--   700301 Apprentice's Greatsword    (Warrior 2H sword)
--   700302 Protector's Mail Vest      (Paladin chest)
--   700303 Apprentice's Mace          (Paladin 1H mace)
--   700304 Scout's Leather Vest       (Hunter chest)
--   700305 Rough Shortbow             (Hunter bow)
--   700306 Cutthroat's Leather Vest   (Rogue chest)
--   700307 Worn Dagger                (Rogue dagger)
--   700308 Acolyte's Robe             (Priest chest)
--   700309 Acolyte's Staff            (Priest staff)
--   700310 Runeguard's Mail Vest      (DK chest)
--   700311 Runeblade of the Initiate  (DK 2H sword)
--   700312 Shaman's Leather Vest      (Shaman chest)
--   700313 Earthen Mace               (Shaman 2H mace)
--   700314 Apprentice's Robe          (Mage chest)
--   700315 Apprentice's Staff         (Mage staff)
--   700316 Fel-Touched Robe           (Warlock chest)
--   700317 Warlock's Staff            (Warlock staff)
--   700318 Druid's Leather Vest       (Druid chest)
--   700319 Ironwood Staff             (Druid staff)
--   700320 Initiate's Blade           (DK 1H sword)
--
-- Replaced by:
--   Mail chest:    26031  Elekk Rider's Mail      (ilvl 11, AllowableClass=-1)
--   Leather chest: 24111  Kurken Hide Jerkin       (ilvl 10, AllowableClass=-1)
--   Cloth chest:   26004  Farmhand's Vest          (ilvl 11, AllowableClass=-1)
--   Warrior 2H:    27389  Surplus Bastard Sword    (ilvl 11, AllowableClass=-1)
--   DK 2H:         27389  Surplus Bastard Sword    (ilvl 11, AllowableClass=-1)
--   DK 1H x2:      18957  Brushwood Blade          (ilvl 10, AllowableClass=-1)
--   Paladin 1H:     4948  Stinging Mace            (ilvl 11, AllowableClass=-1)
--   Hunter bow:    28152  Quel'Thalas Recurve      (ilvl 11, AllowableClass=-1)
--   Rogue dag x2:   4947  Jagged Dagger            (ilvl 11, AllowableClass=-1)
--   Shaman 2H:     26051  2 Stone Sledgehammer     (ilvl 11, AllowableClass=-1)
--   Caster staff:   9603  Gritroot Staff           (ilvl 10, AllowableClass=-1)

DELETE FROM item_template WHERE entry IN (
    700300, 700301, 700302, 700303, 700304, 700305,
    700306, 700307, 700308, 700309, 700310, 700311,
    700312, 700313, 700314, 700315, 700316, 700317,
    700318, 700319, 700320
);
