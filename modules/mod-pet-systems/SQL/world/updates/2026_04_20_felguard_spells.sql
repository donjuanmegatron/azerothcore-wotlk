-- Sanctum: Felguard full spell setup (2026-04-20)
--
-- Felguard (entry 17252) is normally only summonable at level 50+ via deep
-- Demonology talents. In Sanctum, Warlocks can summon it at level 10, which
-- means the default pet spell grants leave it with nothing but melee attacks.
--
-- This SQL configures creature_template_spell for entry 17252 so that when
-- the Felguard is spawned as a combat guardian (TempSummon via SummonCreature),
-- the creature AI knows about its abilities and will cast them.
-- For the real Pet* slot Felguard, spells are force-learned in C++ via
-- EnsureFelguardSpells() in mod-pet-systems.cpp.
--
-- Spell assignments:
--   Index 0: Legion Strike rank 1 (30328) — primary DPS, AOE melee cleave
--   Index 1: Legion Strike rank 4 (30331) — mid-level rank for scaling
--   Index 2: Legion Strike rank 8 (30335) — highest rank (WotLK cap)
--   Index 3: Intercept        (30153)  — charge + 3-sec stun on engage
--   Index 4: Demonic Frenzy   (32850)  — passive stacking attack power buff
--
-- Legion Strike is listed at three ranks so the creature AI selects the
-- appropriate one based on the target's level. In practice the AI uses the
-- first valid spell, so rank 1 will fire most often — but granting the
-- higher ranks ensures the pet bar (real pet slot) shows a scaling ability.
--
-- DamageModifier bump: default is 1.0. Setting to 1.25 gives the Felguard
-- 25% stronger base auto-attacks, compensating for the fact that creature
-- stat tables at level 10 were never tuned for a player-owned demon at that
-- level. The 40% owner AP inheritance in C++ adds on top of this.

DELETE FROM creature_template_spell WHERE creature_entry = 17252;

INSERT INTO creature_template_spell (creature_entry, `index`, Spell) VALUES
(17252, 0, 30328),   -- Legion Strike Rank 1
(17252, 1, 30331),   -- Legion Strike Rank 4
(17252, 2, 30335),   -- Legion Strike Rank 8
(17252, 3, 30153),   -- Intercept
(17252, 4, 32850);   -- Demonic Frenzy (passive)

-- Increase base damage output to compensate for level-10 creature stat table
-- being tuned around level 55–60 encounter design.
-- 1.25 = 25% above baseline. Adjust after in-game testing if needed.
UPDATE creature_template SET DamageModifier = 1.25 WHERE entry = 17252;
