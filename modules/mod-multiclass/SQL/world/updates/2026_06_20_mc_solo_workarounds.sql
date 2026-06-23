-- Sanctum solo workaround: Molten Core (cross-heal softening)
-- Majordomo Flamewaker Healer (11663) and Sulfuron Flamewaker Priest (11662)
-- are elite raid adds (orig HealthModifier 40 / 30). Cut ~60% so a solo player
-- can kill the cross-healers before they out-heal the encounter, WITHOUT
-- trivializing them (they keep substantial HP and the add pressure stays).
-- TUNING KNOB: raise back toward 40/30 if too easy, lower if still a stalemate.
UPDATE `creature_template` SET `HealthModifier` = 16 WHERE `entry` = 11663; -- Flamewaker Healer (was 40)
UPDATE `creature_template` SET `HealthModifier` = 12 WHERE `entry` = 11662; -- Flamewaker Priest (was 30)
