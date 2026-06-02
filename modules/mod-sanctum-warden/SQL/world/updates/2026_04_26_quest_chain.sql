-- Sanctum Creation Quest Chain (2026-04-26)
--
-- Replaces the MailDraft starter gear system with a 4-quest chain
-- that plays out entirely in the creation room.
--
-- Quest 700001: Welcome to Sanctum         — reward: 2 bags (instant turn-in)
-- Quest 700002: Bind Your Second Path      — reward: full weapon arsenal
-- Quest 700003: Bind Your Third Path       — reward: all chest pieces
-- Quest 700004: Where Adventure Begins     — root of the long chain (bags → raids → expansion)
--
-- Objective credit NPCs (never spawned, kill-credit only):
--   700050 = "Second Path Bound"   (class2 binding)
--   700051 = "Third Path Bound"    (class3 binding)
--   700052 = "Starting Zone Chosen"
--   700053 = "Level 10 Reached"

-- ============================================================
-- Dummy creature templates for objective credits
-- These are never spawned in the world — they exist only so
-- KilledMonsterCredit() can reference a valid creature entry.
-- flags_extra = 2 (CREATURE_FLAG_EXTRA_NO_XP_AT_KILL)
-- ============================================================
DELETE FROM creature_template WHERE entry IN (700050, 700051, 700052, 700053);
INSERT INTO creature_template (entry, name, faction, minlevel, maxlevel, flags_extra)
VALUES
  (700050, 'Second Path Bound',    35, 1, 1, 2),
  (700051, 'Third Path Bound',     35, 1, 1, 2),
  (700052, 'Starting Zone Chosen', 35, 1, 1, 2),
  (700053, 'Level 10 Reached',     35, 1, 1, 2);

-- ============================================================
-- Quest 700001: Welcome to Sanctum
-- No objectives — instantly completeable after acceptance.
-- Reward: 2x Foror's Crate (bag placeholder).
-- ============================================================
DELETE FROM quest_template WHERE ID = 700001;
INSERT INTO quest_template (
    ID, QuestType, QuestLevel, MinLevel, QuestSortID,
    RewardNextQuest,
    RewardItem1, RewardAmount1,
    RewardItem2, RewardAmount2,
    RewardMoney,
    AllowableRaces,
    LogTitle, LogDescription, QuestDescription, QuestCompletionLog,
    VerifiedBuild
) VALUES (
    700001, 2, 1, 1, -1,
    700002,
    23162, 1,
    23162, 1,
    50000,
    0,
    'Welcome to Sanctum',
    'Speak with the Sanctum Warden.',
    'I have been watching you for a long time.\n\nThe path ahead will not be easy — but you will not walk it empty-handed. These packs are yours. Keep them close.\n\nYour story is only beginning.',
    'Claim your supplies from the Sanctum Warden.',
    0
);

DELETE FROM quest_template_addon WHERE ID = 700001;
INSERT INTO quest_template_addon (ID, PrevQuestID, NextQuestID, SpecialFlags)
VALUES (700001, 0, 700002, 0);

DELETE FROM quest_offer_reward WHERE ID = 700001;
INSERT INTO quest_offer_reward (ID, RewardText, VerifiedBuild)
VALUES (700001, 'Take these packs. They will serve you well on the road ahead.\n\nNow — you must choose who you are. Ten Class Masters stand in this chamber. Approach the first that calls to you.', 0);

-- ============================================================
-- Quest 700002: Bind Your Second Path
-- Objective: bind class2 at any Class Master [0/1]
-- Reward: all starter weapons, granted via C++ OnQuestReward
-- ============================================================
DELETE FROM quest_template WHERE ID = 700002;
INSERT INTO quest_template (
    ID, QuestType, QuestLevel, MinLevel, QuestSortID,
    RewardNextQuest,
    AllowableRaces,
    RequiredNpcOrGo1, RequiredNpcOrGoCount1,
    ObjectiveText1,
    LogTitle, LogDescription, QuestDescription, QuestCompletionLog,
    VerifiedBuild
) VALUES (
    700002, 2, 1, 1, -1,
    700003,
    0,
    700050, 1,
    'Bind a second class at a Class Master',
    'Bind Your Second Path',
    'Approach a Class Master and bind your second discipline.',
    'Ten Class Masters stand in the Chamber of Paths. Each walks a different road.\n\nApproach one that calls to you and bind yourself to their discipline. You may bind two more. Choose with purpose.',
    'Return to the Sanctum Warden once your second path is bound.',
    0
);

DELETE FROM quest_template_addon WHERE ID = 700002;
INSERT INTO quest_template_addon (ID, PrevQuestID, NextQuestID, SpecialFlags)
VALUES (700002, 700001, 700003, 0);

DELETE FROM quest_offer_reward WHERE ID = 700002;
INSERT INTO quest_offer_reward (ID, RewardText, VerifiedBuild)
VALUES (700002, 'A second path bound. Here is every weapon from my armory — take what suits your hand.\n\nOne choice remains. Return to the masters.', 0);

DELETE FROM quest_request_items WHERE ID = 700002;
INSERT INTO quest_request_items (ID, CompletionText, VerifiedBuild)
VALUES (700002, 'You have bound your second class. Return to claim your reward.', 0);

-- ============================================================
-- Quest 700003: Bind Your Third Path
-- Objective: bind class3 at any Class Master [0/1]
-- Reward: all starter chests, granted via C++ OnQuestReward
-- ============================================================
DELETE FROM quest_template WHERE ID = 700003;
INSERT INTO quest_template (
    ID, QuestType, QuestLevel, MinLevel, QuestSortID,
    RewardNextQuest,
    AllowableRaces,
    RequiredNpcOrGo1, RequiredNpcOrGoCount1,
    ObjectiveText1,
    LogTitle, LogDescription, QuestDescription, QuestCompletionLog,
    VerifiedBuild
) VALUES (
    700003, 2, 1, 1, -1,
    700004,
    0,
    700051, 1,
    'Bind a third class at a Class Master',
    'Bind Your Third Path',
    'Approach a Class Master and bind your third and final discipline.',
    'One path remains unchosen. Return to the Chamber of Paths and bind your third and final class.\n\nOnce bound, your three disciplines are permanent. There is no going back.',
    'Return to the Sanctum Warden once your third path is bound.',
    0
);

DELETE FROM quest_template_addon WHERE ID = 700003;
INSERT INTO quest_template_addon (ID, PrevQuestID, NextQuestID, SpecialFlags)
VALUES (700003, 700002, 700004, 0);

DELETE FROM quest_offer_reward WHERE ID = 700003;
INSERT INTO quest_offer_reward (ID, RewardText, VerifiedBuild)
VALUES (700003, 'All three paths are now bound. Here is every piece of armor from my armory. Wear what your disciplines demand.\n\nNow — go out and begin your story.', 0);

DELETE FROM quest_request_items WHERE ID = 700003;
INSERT INTO quest_request_items (ID, CompletionText, VerifiedBuild)
VALUES (700003, 'You have bound your third class. Return to claim your reward.', 0);

-- ============================================================
-- Quest 700004: Where Adventure Begins
-- This is the root quest of the Sanctum long chain.
-- Objectives: pick a starting zone AND reach level 10.
-- Auto-completes via C++ when both objectives are met.
-- Reward: 1 gold (placeholder — real reward designed later).
-- Future: RewardNextQuest will chain to the first dungeon quest.
-- ============================================================
DELETE FROM quest_template WHERE ID = 700004;
INSERT INTO quest_template (
    ID, QuestType, QuestLevel, MinLevel, QuestSortID,
    RewardMoney,
    AllowableRaces,
    RequiredNpcOrGo1, RequiredNpcOrGoCount1,
    RequiredNpcOrGo2, RequiredNpcOrGoCount2,
    ObjectiveText1, ObjectiveText2,
    LogTitle, LogDescription, QuestDescription, QuestCompletionLog,
    VerifiedBuild
) VALUES (
    700004, 2, 1, 1, -1,
    10000,
    0,
    700052, 1,
    700053, 1,
    'Choose a starting zone',
    'Reach level 10',
    'Where Adventure Begins',
    'Choose a starting zone from the Sanctum Warden, then reach level 10.',
    'You have bound three paths. Now choose where your adventure begins.\n\nSpeak with me to select your starting zone. Once there, prove yourself — reach level 10 and your journey will truly begin.',
    'You have chosen your path and proven your worth. The world of Azeroth opens before you.',
    0
);

DELETE FROM quest_template_addon WHERE ID = 700004;
INSERT INTO quest_template_addon (ID, PrevQuestID, NextQuestID, SpecialFlags)
VALUES (700004, 700003, 0, 2);  -- SpecialFlags=2: completeable by C++ script

DELETE FROM quest_offer_reward WHERE ID = 700004;
INSERT INTO quest_offer_reward (ID, RewardText, VerifiedBuild)
VALUES (700004, 'Your story has truly begun. Azeroth is vast — and yours to claim.', 0);

-- ============================================================
-- Warden (700200) as quest giver and quest ender
-- ============================================================
DELETE FROM creature_queststarter WHERE id = 700200 AND quest IN (700001, 700002, 700003, 700004);
INSERT INTO creature_queststarter (id, quest) VALUES
    (700200, 700001),
    (700200, 700002),
    (700200, 700003),
    (700200, 700004);

-- Quest 700004 auto-completes via C++ — no questender entry needed
DELETE FROM creature_questender WHERE id = 700200 AND quest IN (700001, 700002, 700003);
INSERT INTO creature_questender (id, quest) VALUES
    (700200, 700001),
    (700200, 700002),
    (700200, 700003);
