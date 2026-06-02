-- Class core spell fixes (2026-04-25)
--
-- Two categories of problem when a player picks class2/class3 at a Class Master:
--   1. Spells that exist in trainer_spell but at too high a ReqLevel to grant at level 1
--   2. Spells that are class-defining passives living in playercreateinfo_spell_custom only
--      (skipped for secondary classes) and missing from trainer_spell entirely
--
-- Fix: lower ReqLevel to 1 where needed, and INSERT missing class-core spells
-- into trainer_spell for each class's trainers so GrantClassSpells picks them up.

-- ============================================================
-- ROGUE — Dual Wield (674)
-- Dual Wield is in trainer_spell for Rogues at ReqLevel=10.
-- A player who picks Rogue as class2/class3 at level 1 won't
-- receive it until they revisit the trainer at level 10.
-- Drop it to ReqLevel=1 so it's granted immediately on class bind.
-- ============================================================

UPDATE `trainer_spell` ts
INNER JOIN `trainer` t ON t.Id = ts.TrainerId
SET ts.ReqLevel = 1
WHERE t.Type = 0 AND t.Requirement = 4 AND ts.SpellId = 674;

-- ============================================================
-- PALADIN — Judgment (20271)
-- Judgment is a starting passive for Paladins (playercreateinfo_spell_custom),
-- not in trainer_spell at all. Secondary/tertiary Paladins never receive it
-- because GrantClassSpells only queries trainer_spell for class2/class3.
-- Insert it for all 4 Paladin trainers at ReqLevel=1.
-- ============================================================

INSERT IGNORE INTO `trainer_spell` (`TrainerId`, `SpellId`, `MoneyCost`, `ReqSkillLine`, `ReqSkillRank`, `ReqAbility1`, `ReqAbility2`, `ReqAbility3`, `ReqLevel`, `VerifiedBuild`)
VALUES
    (3, 20271, 10, 0, 0, 0, 0, 0, 1, 0),
    (4, 20271, 10, 0, 0, 0, 0, 0, 1, 0),
    (5, 20271, 10, 0, 0, 0, 0, 0, 1, 0),
    (6, 20271, 10, 0, 0, 0, 0, 0, 1, 0);
