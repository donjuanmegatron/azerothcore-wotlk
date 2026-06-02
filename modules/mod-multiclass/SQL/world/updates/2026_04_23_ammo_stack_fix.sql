-- Sanctum: Raise stack size on Rough Arrow (2512) and Rough Bullet (2519) to 500000.
-- These are the ammo items used by RefillRangedAmmo() — no level requirement, so any
-- character can use them at level 1. Default stack size in vanilla is 200; we need
-- 500000 so the auto-stock system can store a full session's worth in a single stack.
UPDATE item_template SET stackable = 500000 WHERE entry IN (2512, 2519);
