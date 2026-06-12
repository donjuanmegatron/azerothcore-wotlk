-- Sanctum multiclass: remove class restrictions from all weapons and armor.
--
-- Multiclass characters play 3 WoW classes but have only ONE real WoW class
-- under the hood. The core's Player::CanUseItem enforces item_template.AllowableClass
-- against that single real class, so class-locked gear (e.g. Battlegear of Might =
-- Warrior only) is rejected even when the character's chosen classes include it.
--
-- Enforcing "only your 3 chosen classes" would require a core patch to CanUseItem.
-- On a solo/multiclass server the practical equivalent is to open AllowableClass on
-- all weapons (class 2) and armor (class 4) so any character can equip anything.
-- Tier tokens, quest items, trade goods, etc. are intentionally left untouched.
--
-- Applied live 2026-06-10 (8,517 rows). This file makes it persist across reimports.

UPDATE `item_template`
SET `AllowableClass` = -1
WHERE `AllowableClass` <> -1
  AND `class` IN (2, 4);
