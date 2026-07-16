-- mod-power-stones PHASE 3: socket overlay
-- Links an owned stone (character_power_stones.id) to a socket on a specific
-- equipped item, identified by the item's low GUID. This is a PURE Sanctum
-- overlay — it never touches the item's native WoW gem/socket slots, so native
-- gems and Power Stones coexist on the same item.
--
-- A stone can be in at most ONE socket (uq_stone). A socket holds at most one
-- stone (PK on item_guid+socket_index). socket_index is 1-based (Socket 1/2/3).

CREATE TABLE IF NOT EXISTS `character_socketed_stones` (
    `guid`         INT UNSIGNED    NOT NULL,           -- owning character (low GUID)
    `item_guid`    INT UNSIGNED    NOT NULL,           -- the equipped item's low GUID
    `socket_index` TINYINT UNSIGNED NOT NULL,          -- 1-based socket number on that item
    `stone_id`     BIGINT UNSIGNED NOT NULL,           -- FK -> character_power_stones.id
    PRIMARY KEY (`item_guid`, `socket_index`),
    UNIQUE KEY `uq_stone` (`stone_id`),
    KEY `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Sanctum Power Stones socketed into equipped items (overlay, not native gems)';
