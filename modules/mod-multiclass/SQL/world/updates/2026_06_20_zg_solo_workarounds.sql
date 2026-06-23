-- Sanctum solo workaround: Zul'Gurub
-- Jin'do "Powerful Healing Ward" (14987): reduce HP so a solo player can kill it
-- before it out-heals Jin'do. Keeps it as an add; removes the heal-stalemate.
UPDATE `creature_template` SET `HealthModifier` = 0.3 WHERE `entry` = 14987;
