-- Sanctum: neutralise capital city guards so cross-faction players are not attacked.
--
-- Root cause: SetFaction(35) on the player made ALL mobs neutral, including dungeon
-- enemies. Removing it restores dungeon hostility. To keep Sanctum's design goal of
-- faction-free city access, we set city guard NPCs themselves to faction 35 (Friendly).
--
-- Targets: guards / sentinels / peacekeepers / braves / grunts in all capital cities
-- and neutral hubs (Dalaran, Shattrath). Does NOT affect dungeon or world-zone mobs.
--
-- Safe to run multiple times (idempotent WHERE clause).

UPDATE creature_template SET faction = 35 WHERE
    name IN (
        -- Stormwind
        'Stormwind City Guard', 'Stormwind Royal Guard', 'Stormwind Guard',
        -- Ironforge
        'Ironforge Guard', 'Ironforge Mountaineer',
        -- Darnassus
        'Darnassus Sentinel',
        -- Exodar
        'Exodar Peacekeeper',
        -- Orgrimmar
        'Orgrimmar Grunt', 'Orgrimmar Guard',
        -- Thunder Bluff
        'Thunder Bluff Brave', 'Thunder Bluff Watcher',
        -- Undercity
        'Undercity Guardian', 'Deathguard', 'Undercity Deathguard',
        -- Silvermoon
        'Silvermoon City Guardian', 'Silvermoon Guardsman', 'Silvermoon Guardian',
        'Silvermoon City Guard',
        -- Dalaran
        'Dalaran Peacekeeper', 'Dalaran Warden', 'Dalaran Wizard Guard',
        -- Shattrath
        'Shattrath Peacekeeper', 'Sha''tar Peacekeeper',
        'Aldor Peacekeeper', 'Scryer Peacekeeper',
        -- Booty Bay / neutral hub guards (so player can use neutral cities freely)
        'Booty Bay Bruiser', 'Steamwheedle Bruiser',
        'Gadgetzan Peacekeeper', 'Everlook Bruiser', 'Ratchet Bruiser'
    );
