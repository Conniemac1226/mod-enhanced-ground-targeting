-- Legacy manual installer cleanup. New installations use data/sql/world/updates/ automatically.
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_enhanced_ground_targeting';
