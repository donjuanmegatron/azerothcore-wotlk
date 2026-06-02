-- Rank-1 class spell grants for class2/class3 (2026-04-25)
--
-- Root cause: rank-1 class spells are character-creation spells in vanilla WoW,
-- not trainer spells. GrantClassSpells only queries trainer_spell, so class2/class3
-- players were missing every foundational class ability.
--
-- Armor proficiency and secondary resource pools (Rage/Energy) are already handled
-- by GrantAllEquipSkills and the WorldScript RefillSecondaryPower — no SQL needed.
--
-- Trainer ID map (Type=0 class trainers):
--   Warrior  1,2  |  Paladin 3-6   |  Hunter  7,8  |  Rogue   9,10
--   Priest  11,12 |  DK     13     |  Shaman 14,15  |  Mage   16-29
--   Warlock 31,32 |  Druid  33,34

-- ============================================================
-- WARRIOR (trainers 1, 2)
-- Battle Stance (2457): required to generate Rage and use stance-dependent abilities.
-- Heroic Strike R1 (78): basic on-next-attack melee strike (rank 2, spell 284, is at L8).
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (1, 2457, 10, 0,0,0,0,0, 1, 0),  -- Battle Stance
    (2, 2457, 10, 0,0,0,0,0, 1, 0),
    (1,   78, 10, 0,0,0,0,0, 1, 0),  -- Heroic Strike R1
    (2,   78, 10, 0,0,0,0,0, 1, 0);

-- ============================================================
-- PALADIN (trainers 3, 4, 5, 6)
-- Seal of Righteousness R1 (21084): Judgment is useless without an active seal.
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (3, 21084, 10, 0,0,0,0,0, 1, 0),  -- Seal of Righteousness R1
    (4, 21084, 10, 0,0,0,0,0, 1, 0),
    (5, 21084, 10, 0,0,0,0,0, 1, 0),
    (6, 21084, 10, 0,0,0,0,0, 1, 0);

-- ============================================================
-- HUNTER (trainers 7, 8)
-- Hunter's Mark (1494): lower from ReqLevel 2 → 1.
-- Serpent Sting R1 (1978): only ranged damage option at level 1 (lower from L4 → 1).
-- Tame Beast (1515): core identity — can't have a pet without it.
-- ============================================================

UPDATE `trainer_spell` ts
INNER JOIN `trainer` t ON t.Id = ts.TrainerId
SET ts.ReqLevel = 1
WHERE t.Type = 0 AND t.Requirement = 3 AND ts.SpellId IN (1494, 1978);

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (7, 1515, 10, 0,0,0,0,0, 1, 0),  -- Tame Beast
    (8, 1515, 10, 0,0,0,0,0, 1, 0);

-- ============================================================
-- ROGUE (trainers 9, 10)
-- Sinister Strike R1 (1752): primary combo-point generator (R2 is 1757 at L6).
-- Eviscerate R1 (2098): primary finisher.
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (9,  1752, 10, 0,0,0,0,0, 1, 0),  -- Sinister Strike R1
    (10, 1752, 10, 0,0,0,0,0, 1, 0),
    (9,  2098, 10, 0,0,0,0,0, 1, 0),  -- Eviscerate R1
    (10, 2098, 10, 0,0,0,0,0, 1, 0);

-- ============================================================
-- PRIEST (trainers 11, 12)
-- Smite R1 (585): primary damage spell.
-- Lesser Heal R1 (2050): primary heal.
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (11, 585,  10, 0,0,0,0,0, 1, 0),  -- Smite R1
    (12, 585,  10, 0,0,0,0,0, 1, 0),
    (11, 2050, 10, 0,0,0,0,0, 1, 0),  -- Lesser Heal R1
    (12, 2050, 10, 0,0,0,0,0, 1, 0);

-- ============================================================
-- DEATH KNIGHT (trainer 13)
-- Core DK strikes were Acherus quest rewards, not trainer spells.
-- Icy Touch (45477), Plague Strike (45462), Blood Strike (45902), Death Coil (47541).
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (13, 45477, 10, 0,0,0,0,0, 1, 0),  -- Icy Touch R1
    (13, 45462, 10, 0,0,0,0,0, 1, 0),  -- Plague Strike R1
    (13, 45902, 10, 0,0,0,0,0, 1, 0),  -- Blood Strike R1
    (13, 47541, 10, 0,0,0,0,0, 1, 0);  -- Death Coil R1

-- ============================================================
-- SHAMAN (trainers 14, 15)
-- Lightning Bolt R1 (403): primary damage spell.
-- Healing Wave R1 (331): primary heal.
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (14, 403, 10, 0,0,0,0,0, 1, 0),  -- Lightning Bolt R1
    (15, 403, 10, 0,0,0,0,0, 1, 0),
    (14, 331, 10, 0,0,0,0,0, 1, 0),  -- Healing Wave R1
    (15, 331, 10, 0,0,0,0,0, 1, 0);

-- ============================================================
-- MAGE (trainers 16–29)
-- Fireball R1 (133): core damage identity (Frostbolt 116 already at L4 is fine,
-- but Fireball R1 is entirely absent from trainer_spell).
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (16, 133, 10, 0,0,0,0,0, 1, 0),
    (17, 133, 10, 0,0,0,0,0, 1, 0),
    (18, 133, 10, 0,0,0,0,0, 1, 0),
    (19, 133, 10, 0,0,0,0,0, 1, 0),
    (20, 133, 10, 0,0,0,0,0, 1, 0),
    (21, 133, 10, 0,0,0,0,0, 1, 0),
    (22, 133, 10, 0,0,0,0,0, 1, 0),
    (23, 133, 10, 0,0,0,0,0, 1, 0),
    (24, 133, 10, 0,0,0,0,0, 1, 0),
    (25, 133, 10, 0,0,0,0,0, 1, 0),
    (26, 133, 10, 0,0,0,0,0, 1, 0),
    (27, 133, 10, 0,0,0,0,0, 1, 0),
    (28, 133, 10, 0,0,0,0,0, 1, 0),
    (29, 133, 10, 0,0,0,0,0, 1, 0);

-- ============================================================
-- WARLOCK (trainers 31, 32)
-- Shadow Bolt R1 (686): primary damage spell. Imp (688) is already at L1 ✓.
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (31, 686, 10, 0,0,0,0,0, 1, 0),  -- Shadow Bolt R1
    (32, 686, 10, 0,0,0,0,0, 1, 0);

-- ============================================================
-- DRUID (trainers 33, 34)
-- Wrath R1 (5176): primary damage spell.
-- Healing Touch R1 (5185): primary heal.
-- Rejuvenation (774) and Moonfire (8921) are already at L4 ✓.
-- ============================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`,`SpellId`,`MoneyCost`,`ReqSkillLine`,`ReqSkillRank`,`ReqAbility1`,`ReqAbility2`,`ReqAbility3`,`ReqLevel`,`VerifiedBuild`)
VALUES
    (33, 5176, 10, 0,0,0,0,0, 1, 0),  -- Wrath R1
    (34, 5176, 10, 0,0,0,0,0, 1, 0),
    (33, 5185, 10, 0,0,0,0,0, 1, 0),  -- Healing Touch R1
    (34, 5185, 10, 0,0,0,0,0, 1, 0);
