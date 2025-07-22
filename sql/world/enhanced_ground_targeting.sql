-- Clean up any existing entries
DELETE FROM spell_script_names WHERE ScriptName = 'spell_enhanced_ground_targeting';

-- Add entries for spells that require manual ground targeting

-- WARLOCK SPELLS
INSERT INTO spell_script_names (spell_id, ScriptName) VALUES 
(5740, 'spell_enhanced_ground_targeting'),   -- Rain of Fire Rank 1
(6219, 'spell_enhanced_ground_targeting'),   -- Rain of Fire Rank 2
(11677, 'spell_enhanced_ground_targeting'),  -- Rain of Fire Rank 3
(11678, 'spell_enhanced_ground_targeting'),  -- Rain of Fire Rank 4
(27212, 'spell_enhanced_ground_targeting'),  -- Rain of Fire Rank 5
(47819, 'spell_enhanced_ground_targeting'),  -- Rain of Fire Rank 6
(47820, 'spell_enhanced_ground_targeting'),  -- Rain of Fire Rank 7
(18540, 'spell_enhanced_ground_targeting'),  -- Summon Doomguard
(1122, 'spell_enhanced_ground_targeting'),   -- Summon Infernal

-- MAGE SPELLS
(10, 'spell_enhanced_ground_targeting'),      -- Blizzard Rank 1
(6141, 'spell_enhanced_ground_targeting'),    -- Blizzard Rank 2
(8427, 'spell_enhanced_ground_targeting'),    -- Blizzard Rank 3
(10185, 'spell_enhanced_ground_targeting'),   -- Blizzard Rank 4
(10186, 'spell_enhanced_ground_targeting'),   -- Blizzard Rank 5
(10187, 'spell_enhanced_ground_targeting'),   -- Blizzard Rank 6
(27085, 'spell_enhanced_ground_targeting'),   -- Blizzard Rank 7
(42939, 'spell_enhanced_ground_targeting'),   -- Blizzard Rank 8
(42940, 'spell_enhanced_ground_targeting'),   -- Blizzard Rank 9

(2120, 'spell_enhanced_ground_targeting'),    -- Flamestrike Rank 1
(2121, 'spell_enhanced_ground_targeting'),    -- Flamestrike Rank 2
(8422, 'spell_enhanced_ground_targeting'),    -- Flamestrike Rank 3
(8423, 'spell_enhanced_ground_targeting'),    -- Flamestrike Rank 4
(10215, 'spell_enhanced_ground_targeting'),   -- Flamestrike Rank 5
(10216, 'spell_enhanced_ground_targeting'),   -- Flamestrike Rank 6
(27086, 'spell_enhanced_ground_targeting'),   -- Flamestrike Rank 7
(42925, 'spell_enhanced_ground_targeting'),   -- Flamestrike Rank 8
(42926, 'spell_enhanced_ground_targeting'),   -- Flamestrike Rank 9

-- PRIEST SPELLS
(48045, 'spell_enhanced_ground_targeting'),   -- Mind Sear Rank 1
(53023, 'spell_enhanced_ground_targeting'),   -- Mind Sear Rank 2
(49821, 'spell_enhanced_ground_targeting'),   -- Mind Sear Rank 1 (incorrect rank)
(53022, 'spell_enhanced_ground_targeting'),   -- Mind Sear Rank 2 (incorrect rank)

(32375, 'spell_enhanced_ground_targeting'),   -- Mass Dispel

-- DRUID SPELLS
(16914, 'spell_enhanced_ground_targeting'),   -- Hurricane Rank 1
(17401, 'spell_enhanced_ground_targeting'),   -- Hurricane Rank 2
(17402, 'spell_enhanced_ground_targeting'),   -- Hurricane Rank 3
(27012, 'spell_enhanced_ground_targeting'),   -- Hurricane Rank 4
(48466, 'spell_enhanced_ground_targeting'),   -- Hurricane Rank 5

(50288, 'spell_enhanced_ground_targeting'),   -- Starfall Rank 1
(53188, 'spell_enhanced_ground_targeting'),   -- Starfall Rank 2
(53189, 'spell_enhanced_ground_targeting'),   -- Starfall Rank 3
(53190, 'spell_enhanced_ground_targeting'),   -- Starfall Rank 4

-- HUNTER SPELLS
(1510, 'spell_enhanced_ground_targeting'),    -- Volley Rank 1
(14294, 'spell_enhanced_ground_targeting'),   -- Volley Rank 2
(14295, 'spell_enhanced_ground_targeting'),   -- Volley Rank 3
(27022, 'spell_enhanced_ground_targeting'),   -- Volley Rank 4
(58431, 'spell_enhanced_ground_targeting'),   -- Volley Rank 5
(58432, 'spell_enhanced_ground_targeting'),   -- Volley Rank 6

-- DEATH KNIGHT SPELLS
(43265, 'spell_enhanced_ground_targeting'),   -- Death and Decay Rank 1
(49936, 'spell_enhanced_ground_targeting'),   -- Death and Decay Rank 2
(49937, 'spell_enhanced_ground_targeting'),   -- Death and Decay Rank 3
(49938, 'spell_enhanced_ground_targeting'),   -- Death and Decay Rank 4

(42650, 'spell_enhanced_ground_targeting'),   -- Army of the Dead

-- ROGUE SPELLS
(1725, 'spell_enhanced_ground_targeting');    -- Distract