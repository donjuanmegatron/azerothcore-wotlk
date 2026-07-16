-- mod-conquest-shards: Sanctum Conquest Shard wallet
-- Server-wide alt-currency earned from boss kills, spent on power stones,
-- inscriptions, PvP recipes, gems, GXP boosts, etc. (see project_conquest_shard.md).
--
-- Two tables per character:
--   character_conquest_shards — current balance
--   conquest_shard_ledger     — full audit trail of every award/spend

CREATE TABLE IF NOT EXISTS `character_conquest_shards` (
    `guid`    INT UNSIGNED NOT NULL,
    `balance` BIGINT UNSIGNED NOT NULL DEFAULT 0,   -- current Conquest Shard balance
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Sanctum Conquest Shard wallet balance per character';

CREATE TABLE IF NOT EXISTS `conquest_shard_ledger` (
    `id`     BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `guid`   INT UNSIGNED NOT NULL,                 -- character this entry belongs to
    `delta`  BIGINT NOT NULL,                       -- signed change; negative = spend
    `reason` VARCHAR(64) NOT NULL,                  -- short tag, e.g. "raid-boss", "gxp-boost", "gm"
    `ts`     TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
  COMMENT='Sanctum Conquest Shard audit trail (every award/spend)';
