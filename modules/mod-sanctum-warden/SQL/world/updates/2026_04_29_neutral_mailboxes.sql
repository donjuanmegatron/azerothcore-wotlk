-- Sanctum: make all mailboxes faction-neutral so any race can use them.
-- The WoW client checks faction client-side and silently drops clicks on
-- faction-restricted GOs. Faction 35 = friendly to everyone.
UPDATE gameobject_template_addon
SET faction = 35
WHERE entry IN (
    SELECT entry FROM gameobject_template WHERE type = 19
);
