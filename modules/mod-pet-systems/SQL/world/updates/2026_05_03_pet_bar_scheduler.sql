-- Sanctum: Pet bar scheduler setup (2026-05-03)
--
-- PetBarsWorldScript in mod-pet-systems handles all ability autocasting for
-- Warlock Felguard (17252) and DK Risen Ghoul (26125) guardian creatures.
-- Remove any creature_template_spell entries for both so the vanilla creature AI
-- does not double-cast and interfere with the scheduler's cooldown tracking.

DELETE FROM creature_template_spell WHERE CreatureID IN (17252, 26125);
