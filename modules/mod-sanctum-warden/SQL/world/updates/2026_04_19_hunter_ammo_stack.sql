-- Sanctum: raise stackable cap on WotLK arrows/bullets so hunters can hold
-- 500,000 in a single bag slot.  RefillHunterAmmo() in mod-multiclass tops
-- up to 500k on every login and level-up — no manual ammo management needed.
-- Saronite Razorheads: 41165 (ilvl 200, arrow, subclass 2)
-- Mammoth Cutters:     41164 (ilvl 200, bullet/bolt, subclass 3)
UPDATE item_template SET Stackable = 999999 WHERE entry IN (41164, 41165);
