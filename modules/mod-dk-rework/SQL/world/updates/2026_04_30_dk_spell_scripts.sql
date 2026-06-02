-- mod_dk_rework_spell_scripts.sql
-- Bind SpellScript/AuraScript loaders to DK spell IDs.
-- The ScriptName string must match the name passed to new spell_dk_xxx() in C++.
--
-- Icy Touch      R1-R5: 45477, 49896, 49903, 49904, 49909
-- Plague Strike  R1-R6: 45462, 49917, 49918, 49919, 49920, 49921
-- Frost Strike   R1-R6: 49143, 51416, 51417, 51418, 51419, 55268
-- Heart Strike   R1-R6: 55050, 55258, 55259, 55260, 55261, 55262
-- Scourge Strike R1-R4: 55090, 55265, 55270, 55271
-- Frost Fever disease:  55095
-- Blood Plague disease: 55078

DELETE FROM spell_script_names WHERE spell_id IN (
    45477, 49896, 49897, 49898, 49899, 49903, 49904, 49909,
    45462, 49917, 49918, 49919, 49920, 49921,
    49143, 51416, 51417, 51418, 51419, 55268,
    55050, 55258, 55259, 55260, 55261, 55262,
    55090, 55265, 55266, 55267, 55270, 55271,
    55095, 55078
);

INSERT INTO spell_script_names (spell_id, ScriptName) VALUES
-- Icy Touch R1-R5
(45477, 'spell_dk_icy_touch'),
(49896, 'spell_dk_icy_touch'),
(49903, 'spell_dk_icy_touch'),
(49904, 'spell_dk_icy_touch'),
(49909, 'spell_dk_icy_touch'),
-- Plague Strike R1-R6
(45462, 'spell_dk_plague_strike'),
(49917, 'spell_dk_plague_strike'),
(49918, 'spell_dk_plague_strike'),
(49919, 'spell_dk_plague_strike'),
(49920, 'spell_dk_plague_strike'),
(49921, 'spell_dk_plague_strike'),
-- Frost Strike R1-R6
(49143, 'spell_dk_frost_strike'),
(51416, 'spell_dk_frost_strike'),
(51417, 'spell_dk_frost_strike'),
(51418, 'spell_dk_frost_strike'),
(51419, 'spell_dk_frost_strike'),
(55268, 'spell_dk_frost_strike'),
-- Heart Strike R1-R6
(55050, 'spell_dk_heart_strike'),
(55258, 'spell_dk_heart_strike'),
(55259, 'spell_dk_heart_strike'),
(55260, 'spell_dk_heart_strike'),
(55261, 'spell_dk_heart_strike'),
(55262, 'spell_dk_heart_strike'),
-- Scourge Strike R1-R4
(55090, 'spell_dk_scourge_strike_custom'),
(55265, 'spell_dk_scourge_strike_custom'),
(55270, 'spell_dk_scourge_strike_custom'),
(55271, 'spell_dk_scourge_strike_custom'),
-- Frost Fever disease
(55095, 'spell_dk_frost_fever'),
-- Blood Plague disease
(55078, 'spell_dk_blood_plague');

-- Remove vanilla spell_bonus_data entries for Icy Touch.
-- Vanilla had an AP coefficient baked into spell_bonus_data designed for level 55 DKs.
-- Our SpellScript handles SP scaling manually so these rows would double-dip the bonus.
DELETE FROM spell_bonus_data WHERE entry IN (45477, 49896, 49903, 49904, 49909);
