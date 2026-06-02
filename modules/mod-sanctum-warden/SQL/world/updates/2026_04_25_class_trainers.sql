-- Sanctum Class Trainer NPCs (2026-04-25)
--
-- The Sanctum Warden no longer handles class selection or spell training.
-- Ten Class Master NPCs (700110–700119, one per WoW class) take over:
--   - Players visit the trainer for the class they want to add
--   - Trainer gives flavour dialogue, then a confirmation binds the class
--   - Same trainer handles all future spell training for that class
--
-- All trainers are spawned in Dalaran near the Warden (map 571).
-- They will later move to The Park, Stormwind as part of the Sanctum Hub build.
--
-- This file also:
--   - Removes the mailbox NPC flag from the Sanctum Warden (mailbox feature removed)
--   - Removes the custom starting-zone mailbox game objects (GUIDs 700100–700107)

-- ============================================================
-- 1. Remove starting-zone mailbox game objects
-- ============================================================

DELETE FROM `gameobject` WHERE `guid` IN (700100, 700101, 700102, 700103, 700104, 700105, 700106, 700107);

-- ============================================================
-- 2. Strip mailbox flag from Sanctum Warden
--    Old npcflag: 67108865 (GOSSIP | MAILBOX)
--    New npcflag: 1 (GOSSIP only)
-- ============================================================

UPDATE `creature_template` SET `npcflag` = 1 WHERE `entry` = 700200;

-- ============================================================
-- 3. Class Trainer NPC text (flavor dialogue + states)
--
-- 700210–700219 = intro text when player opens gossip for each class
-- 700220 = confirmation prompt ("are you certain?")
-- 700221 = "path full" message (player already has 3 classes)
-- 700222 = training state heading ("What would you learn today?")
-- ============================================================

DELETE FROM `npc_text` WHERE `ID` IN (700210,700211,700212,700213,700214,700215,700216,700217,700218,700219,700220,700221,700222);

INSERT INTO `npc_text` (`ID`, `text0_0`) VALUES
(700210, 'Aye, I''m Muradin Bronzebeard, and I''ve stood on battlefields that would turn your hair white. The Warrior''s path is the oldest path there is — blade, shield, and the will to outlast every enemy that ever drew breath.\n\nYou want that kind of strength? Come. Train.'),
(700211, 'The Light does not choose the worthy by birth or title. It chooses those who choose it.\n\nI am Tirion Fordring. The Paladin''s oath demands equal parts courage and compassion. If you are prepared to carry that weight, speak the words and I will seal the covenant.'),
(700212, 'I have tracked creatures across every continent and into the Black Temple itself. The Hunter''s craft is patience — knowing your prey before it knows you.\n\nProve you are serious and I will show you what the wilderness has to teach.'),
(700213, 'Every shadow hides an opportunity. Every crowd conceals an exit.\n\nI am Mathias Shaw. The Rogue does not conquer by force — we conquer by precision. If that suits your temperament, say so.'),
(700214, 'The Stars have spoken of you. I am Tyrande Whisperwind, high priestess of Elune.\n\nThe Priest''s path is the most demanding of all — you must learn to hold others alive while standing in the jaws of death yourself. If your devotion is true, Elune will fill your hands with light and shadow alike.'),
(700215, 'Do not mistake what I offer for heroism. The Death Knight was forged in service to the Lich King and now walks in defiance of him.\n\nI am Darion Mograine. There is no turning back from this power once it is yours.'),
(700216, 'The elements do not care who calls them. They care only whether the caller has the discipline to listen.\n\nI am Thrall, and the Shaman''s way is a lifelong conversation with fire, earth, wind, and water. If you are willing to listen as much as you demand, I will teach you their language.'),
(700217, 'Magic is not something you learn. It is something you negotiate — carefully, precisely, with consequences you own entirely.\n\nI am Rhonin. The Mage who forgets that arcane power has a price usually does not survive to forget it twice. Step forward if you understand that.'),
(700218, 'Power is simply the willingness to reach further than others dare.\n\nThe Warlock does not borrow power from demons — the Warlock commands it. If that distinction matters to you, you are already thinking correctly.'),
(700219, 'The earth holds the memory of every living thing that ever walked upon it. I am Cairne Bloodhoof.\n\nThe Druid''s path is that memory made active — healing, growth, the patient cycle of seasons made into action. Come. Let us speak of what it means to walk the old ways.'),
(700220, 'There is no undoing this. Once bound, this class becomes part of who you are for the rest of your life.\n\nAre you certain you wish to walk this path?'),
(700221, 'You walk three paths already. Your soul cannot hold more.\n\nThe three classes you have bound define you now.'),
(700222, 'What would you learn today?');

-- ============================================================
-- 4. Class Trainer NPC templates (creature_template)
--
-- entry   700110–700119 (one per WoW class)
-- npcflag 1     = GOSSIP (custom gossip handles everything)
-- faction 35    = Friendly to all
-- flags_extra 2 = Civilian (won't attack)
-- exp     2     = WotLK
-- ScriptName must match CreatureScript("npc_sanctum_class_trainer") in the cpp
-- ============================================================

DELETE FROM `creature_template` WHERE `entry` IN (700110,700111,700112,700113,700114,700115,700116,700117,700118,700119);

INSERT INTO `creature_template`
    (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`,
     `npcflag`, `speed_walk`, `speed_run`, `unit_class`,
     `type`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
    (700110, 'Muradin Bronzebeard', 'Master of Warriors',   80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700111, 'Tirion Fordring',     'Master of Paladins',   80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700112, 'Maiev Shadowsong',    'Master of Hunters',    80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700113, 'Mathias Shaw',        'Master of Rogues',     80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700114, 'Tyrande Whisperwind', 'Master of Priests',    80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700115, 'Darion Mograine',     'Master of Death Knights', 80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700116, 'Thrall',              'Master of Shamans',    80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700117, 'Rhonin',              'Master of Mages',      80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700118, 'Kel''Thuzad',         'Master of Warlocks',   80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer'),
    (700119, 'Cairne Bloodhoof',    'Master of Druids',     80, 80, 2, 35, 1, 1.0, 1.14286, 1, 7, 1, 2, 'npc_sanctum_class_trainer');

-- ============================================================
-- 5. Display models (creature_template_model)
--
-- Muradin Bronzebeard = 30508   (armored dwarf warrior)
-- Tirion Fordring     = 22209   (human paladin in full plate)
-- Maiev Shadowsong    = 20628   (night elf warden — closest to hunter archetype)
-- Mathias Shaw        = 1736    (human rogue)
-- Tyrande Whisperwind = 7274    (night elf priestess)
-- Darion Mograine     = 25444   (death knight in full DK armor)
-- Thrall              = 27744   (orc shaman — confirmed by Donnie)
-- Rhonin              = 16024   (human archmage)
-- Kel'Thuzad          = 19548   (robed warlock lich — Warlock archetype)
-- Cairne Bloodhoof    = 4307    (tauren elder — confirmed by Donnie)
-- ============================================================

DELETE FROM `creature_template_model` WHERE `CreatureID` IN (700110,700111,700112,700113,700114,700115,700116,700117,700118,700119);

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`)
VALUES
    (700110, 0, 30508, 1.0, 1.0),   -- Warrior: Muradin
    (700111, 0, 22209, 1.0, 1.0),   -- Paladin: Tirion
    (700112, 0, 20628, 1.0, 1.0),   -- Hunter: Maiev
    (700113, 0,  1736, 1.0, 1.0),   -- Rogue: Mathias Shaw
    (700114, 0,  7274, 1.0, 1.0),   -- Priest: Tyrande
    (700115, 0, 25444, 1.0, 1.0),   -- Death Knight: Darion
    (700116, 0, 27744, 1.0, 1.0),   -- Shaman: Thrall (orc)
    (700117, 0, 16024, 1.0, 1.0),   -- Mage: Rhonin
    (700118, 0, 19548, 1.0, 1.0),   -- Warlock: Kel'Thuzad
    (700119, 0,  4307, 1.0, 1.0);   -- Druid: Cairne (tauren)

-- ============================================================
-- 6. Class Trainer spawns — The Park, Stormwind City
--
-- Warden is at (-8754, 1107, 92.0) at the center of The Park circle.
-- 10 Class Masters arranged in a circle (radius 10) around the Warden.
-- Each trainer faces inward toward the Warden at the center.
--
-- Positions verified against existing NPC data in The Park area.
-- Adjust with .npc move in-game if any float or clip into terrain.
-- Z values in this area range 90-94 — use .gps if one needs adjusting.
-- ============================================================

DELETE FROM `creature` WHERE `id1` IN (700110,700111,700112,700113,700114,700115,700116,700117,700118,700119);

INSERT INTO `creature` (`id1`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`)
VALUES
    (700110, 0, -8754.00, 1117.00, 92.0, 4.71, 300),   -- Warrior  (North,  faces South)
    (700111, 0, -8748.00, 1115.00, 92.0, 4.08, 300),   -- Paladin  (NNE,   faces SSW)
    (700112, 0, -8744.00, 1110.00, 92.0, 3.45, 300),   -- Hunter   (ENE,   faces WSW)
    (700113, 0, -8744.00, 1104.00, 92.0, 2.79, 300),   -- Rogue    (ESE,   faces WNW)
    (700114, 0, -8748.00, 1099.00, 92.0, 2.16, 300),   -- Priest   (SSE,   faces NNW)
    (700115, 0, -8754.00, 1097.00, 92.0, 1.57, 300),   -- Death Knight (South, faces North)
    (700116, 0, -8760.00, 1099.00, 92.0, 0.94, 300),   -- Shaman   (SSW,   faces NNE)
    (700117, 0, -8764.00, 1104.00, 92.0, 0.31, 300),   -- Mage     (WSW,   faces ENE)
    (700118, 0, -8764.00, 1110.00, 92.0, 5.90, 300),   -- Warlock  (WNW,   faces ESE)
    (700119, 0, -8760.00, 1115.00, 92.0, 5.27, 300);   -- Druid    (NNW,   faces SSE)
