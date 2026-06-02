// mod-xp-system.cpp
//
// Sanctum XP System — dungeon and raid instance XP bonuses.
//
// Normal kills and quests: default AzerothCore rates (no multiplier).
//
// INSTANCE KILL BONUSES (applied on top of base rate via OnPlayerCreatureKill)
//   Dungeon trash: 1.5x total  (+0.5x bonus given after base)
//   Dungeon boss:  5.0x total  (+4.0x bonus given after base)
//   Raid bosses use the same boss threshold (CREATURE_ELITE_WORLDBOSS rank).
//
// HOOK ORDERING (design note)
//   mod-aa-system hooks OnPlayerGiveXP for the XP bleed. If mod-xp-system
//   also hooked OnPlayerGiveXP for dungeon bonuses, ordering would matter.
//   Instead, OnPlayerCreatureKill fires after base XP + bleed are done, then
//   awards bonus XP via sScriptMgr->OnPlayerGiveXP + GiveXP. This re-runs
//   the bleed on the bonus chunk, so AA pool gets its share of dungeon XP too.

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Map.h"
#include "Formulas.h"
#include "Log.h"

// Bonus ratios (multiplied by rawXp, added on top of base 1.0x already given)
//   Dungeon trash: 1.5x total → bonus = rawXp × 0.5
//   Dungeon boss:  5.0x total → bonus = rawXp × 4.0
static constexpr float DUNGEON_TRASH_BONUS = 0.5f;
static constexpr float DUNGEON_BOSS_BONUS  = 4.0f;

class mod_xp_system_world : public WorldScript
{
public:
    mod_xp_system_world() : WorldScript("mod_xp_system_world") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LOG_INFO("module", "[mod-xp-system] Loaded. Dungeon trash: 1.5x | Boss: 5.0x");
    }
};

class mod_xp_system_player : public PlayerScript
{
public:
    mod_xp_system_player() : PlayerScript("mod_xp_system_player") {}

    void OnPlayerCreatureKill(Player* player, Creature* creature) override
    {
        if (!player || !creature)
            return;

        Map* map = player->GetMap();
        if (!map || (!map->IsDungeon() && !map->IsRaid()))
            return;

        // rawXp = pre-rate base XP (0 for grey-con creatures)
        uint32 rawXp = Acore::XP::Gain(player, creature);
        if (rawXp == 0)
            return;

        CreatureTemplate const* cInfo = creature->GetCreatureTemplate();
        bool isBoss = cInfo && (cInfo->rank == CREATURE_ELITE_WORLDBOSS);

        float bonusRatio = isBoss ? DUNGEON_BOSS_BONUS : DUNGEON_TRASH_BONUS;
        uint32 bonusXp   = (uint32)((float)rawXp * bonusRatio);
        if (bonusXp == 0)
            return;

        // Route through OnPlayerGiveXP so AA bleed applies to this bonus chunk.
        // Player::GiveXP does not call the hook internally — callers must do it.
        sScriptMgr->OnPlayerGiveXP(player, bonusXp, creature, 0);
        if (bonusXp > 0)
            player->GiveXP(bonusXp, creature);
    }
};

void AddSC_mod_xp_system()
{
    new mod_xp_system_world();
    new mod_xp_system_player();
}
