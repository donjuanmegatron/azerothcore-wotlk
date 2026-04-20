-- mod-dk-rework: Add Frost Presence (48266) to DK class trainer at level 20.
--
-- Frost Presence was missing from the trainer_spell table entirely.
-- Blood Presence (48263) is at level 7 and Unholy Presence (48265) is at level 48.
-- Frost Presence is inserted here at level 20 to slot between them.
--
-- TrainerId 13 = the single DK class trainer entry (Requirement=6, Type=0).

INSERT IGNORE INTO `trainer_spell` (`TrainerId`, `SpellId`, `MoneyCost`, `ReqSkillLine`, `ReqSkillRank`, `ReqLevel`)
VALUES (13, 48266, 0, 0, 0, 20);
