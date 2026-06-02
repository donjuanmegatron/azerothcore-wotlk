-- Sanctum: Starter bags for new characters.
--
-- Uses real WoW item ID 23162 (Foror's Crate of Endless Resist Gear Storage, 36 slots).
--
-- Custom item IDs for bags are NOT viable in WoW 3.3.5a:
--   The client only renders bag frame icons for items present in its local MPQ data.
--   Custom IDs (including those below 65535) always show a red ? in the bag frame
--   corner regardless of server item_template data or displayid correctness.
--   The only fix is to use real WoW item IDs the client already knows locally.
--
-- 23162 (Foror's Crate) is the largest non-GM real bag in WotLK at 36 slots.
-- Two copies are mailed to every new character at zone selection via GrantStarterGear.
-- No item_template changes needed — 23162 already exists in the default DB.
--
-- NOTE: AzerothCore MAX_BAG_SIZE = 36 (Bag.h). ContainerSlots > 36 causes Bag::Create
-- to silently return false. 36 is the absolute maximum for any bag item.

-- Clean up all leftover custom bag entries from earlier iterations
DELETE FROM item_template WHERE entry IN (60001, 60002, 700400, 700401);
