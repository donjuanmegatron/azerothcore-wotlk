-- Sanctum: Remove all class spec skill lines from playercreateinfo_skills.
--
-- Root cause: AzerothCore's character creation calls SetSkill() for all 3 spec
-- lines (e.g. Affliction/Demonology/Destruction for Warlock). SetSkill triggers
-- learnSkillRewardedSpells(), which auto-grants every spell in each spec line —
-- all ranks, all levels — the moment the character is created.
--
-- Fix: Delete these entries. mod-multiclass GrantClassSpells() handles granting
-- level-appropriate spells on login/levelup via the trainer_spell table with a
-- ReqLevel filter instead.
--
-- DK weapon/armor/riding skills (129, 229, 293, 762) are intentionally kept.
-- Multi-class weapon/armor entries (daggers, maces, staves, etc.) are kept.

DELETE FROM playercreateinfo_skills WHERE skill IN (
    -- Warrior: Arms, Fury, Protection
    26, 256, 257,
    -- Paladin: Retribution, Protection, Holy
    184, 267, 594,
    -- Hunter: Beast Mastery, Survival, Marksmanship
    50, 51, 163,
    -- Rogue: Combat, Subtlety, Assassination
    38, 39, 253,
    -- Priest: Holy, Shadow, Discipline
    56, 78, 613,
    -- Death Knight: Blood, Frost, Unholy
    770, 771, 772,
    -- Shaman: Enhancement, Restoration, Elemental
    373, 374, 375,
    -- Mage: Frost, Fire, Arcane
    6, 8, 237,
    -- Warlock: Demonology, Affliction, Destruction
    354, 355, 593,
    -- Druid: Feral, Restoration, Balance
    134, 573, 574
);
