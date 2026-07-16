// sanctum_shards.h
//
// Public Spend API for the Sanctum Conquest Shard wallet (mod-conquest-shards).
//
// Conquest Shards are a server-wide alt-currency wallet, earned from boss kills
// (dungeon/raid) and spent on future shard shops: power stones, inscriptions,
// PvP recipes, gems, GXP boosts, etc. (see memory/project_conquest_shard.md).
//
// This header is the ONLY thing other modules should include to interact with
// the wallet. It mirrors the free-function export style used by mod-gear-tiers'
// GearTiers_AddGXP — no class, no singleton, just plain functions implemented
// in mod-conquest-shards.cpp.
//
// Usage from another module:
//   #include "sanctum_shards.h"   // (copy this file or add an include path)
//   or simply: extern bool Shards_TrySpend(Player* player, uint32 amount, char const* reason);
//
#ifndef SANCTUM_SHARDS_H
#define SANCTUM_SHARDS_H

#include "Define.h"

class Player;

// Returns the current Conquest Shard balance for the given character (low-part
// GUID). Returns 0 if the character has never earned/spent shards (no cache
// entry and no DB row).
int64 Shards_GetBalance(uint32 lowGuid);

// Attempts to spend `amount` shards from player's wallet for `reason`.
// Returns false (no state change) if the player is null or the balance is
// insufficient. On success: deducts the balance, writes a ledger row with a
// negative delta, saves to the DB, and sends the player a confirmation message.
bool Shards_TrySpend(Player* player, uint32 amount, char const* reason);

// Awards `amount` shards to player's wallet for `reason`. Updates the cache,
// writes a ledger row with a positive delta, saves to the DB, and sends the
// player a confirmation message. No-op if player is null or amount is 0.
void Shards_Award(Player* player, uint32 amount, char const* reason);

#endif // SANCTUM_SHARDS_H
