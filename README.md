# Enhanced Ground Targeting Module

## Overview
This module enhances ground-targeted spells in AzerothCore by providing intelligent AOE placement that maximizes enemy coverage, similar to how playerbots calculate optimal positioning.

## Features

### Smart Positioning Algorithm
- **Density Analysis**: Scans all nearby enemies to find the largest cluster
- **Optimal Placement**: Calculates the center point of enemy clusters for maximum AOE efficiency
- **Mathematical Precision**: Uses bounding box calculations to determine the best position
- **Fallback System**: Automatically falls back to target positioning for single enemies
- **Cursor Independence**: Works regardless of cursor position - no more error sounds from invalid cursor placement
- **Range Validation**: Automatically adjusts positions to be within spell range

### Supported Spells
The module works with all ground-targeted spells registered in the SQL file:
- **Hunter**: Volley (all ranks)
- **Mage**: Blizzard, Flamestrike (all ranks)  
- **Warlock**: Rain of Fire, Summon Infernal/Doomguard
- **Death Knight**: Death and Decay, Army of the Dead
- **Priest**: Mass Dispel, Mind Sear
- **Druid**: Hurricane, Starfall
- **Rogue**: Distract

### Configuration Options

#### Core Settings
- `EnhancedGroundTargeting.Enable` - Enable/disable the module
- `EnhancedGroundTargeting.AutoTarget` - Enable auto-targeting functionality
- `EnhancedGroundTargeting.CombatOnly` - Only affect combat enemies

#### Smart Positioning Settings
- `EnhancedGroundTargeting.SmartPositioning` - Enable playerbot-style smart positioning
- `EnhancedGroundTargeting.MinEnemiesForSmart` - Minimum enemies required for smart positioning

### Player Commands
- `.toggle` - Toggle enhanced ground targeting on/off
- `.toggle on/enable/1` - Enable the feature
- `.toggle off/disable/0` - Disable the feature

## How It Works

### Smart Positioning Algorithm
1. **Enemy Detection**: Scans for all unfriendly units within 35 yards
2. **Cluster Analysis**: For each enemy, counts how many other enemies are within 2x AOE radius
3. **Density Calculation**: Finds the enemy cluster with the highest density
4. **Optimal Positioning**: Calculates the center point of the largest cluster using bounding box method
5. **Fallback Logic**: If no cluster is found or smart positioning is disabled, uses current target position

### Positioning Logic
```cpp
// Multi-enemy scenario: Calculate optimal cluster center
if (smartPositioning && enemyCount >= minEnemiesForSmart)
{
    AOEPosition optimalPos = CalculateOptimalAOEPosition(player, aoeRadius);
    // Use optimal position for maximum coverage
}
else
{
    // Fallback to current target position
    Unit* target = player->GetSelectedUnit();
    // Use target's position
}
```

### GM Debug Messages
GMs receive detailed debug information:
- Position type (OPTIMAL CLUSTER vs target)
- Exact coordinates (X, Y, Z)
- Number of enemies that will be hit
- Spell name and rank

## Installation

1. Copy the module to your `modules/` directory
2. Configure your desired settings in `EnhancedGroundTargeting.conf`
3. Apply the SQL file to register spells: `sql/world/enhanced_ground_targeting.sql`
4. Build and restart your server

## Benefits

### For Players
- **Improved AOE Efficiency**: Automatically maximizes the number of enemies hit
- **Reduced Micromanagement**: No need to manually position ground-targeted spells
- **Consistent Performance**: Eliminates human error in AOE positioning
- **Player Control**: Can be toggled on/off per player preference
- **Cursor Freedom**: Cast spells anywhere without worrying about cursor position
- **No Error Sounds**: Eliminates frustrating error sounds from invalid cursor placement

### For Server Performance
- **Efficient Algorithms**: Uses optimized enemy scanning and position calculation
- **Configurable Options**: Admins can adjust behavior to match server needs
- **Thread-Safe**: Uses proper locking mechanisms for player state management

## Technical Details

### Based on Playerbot Research
The smart positioning algorithm is based on the playerbot AOE placement system:
- Uses the same density calculation methods as `AoeValues.cpp`
- Implements the same bounding box center calculation
- Follows the same enemy clustering logic

### Performance Considerations
- Enemy scanning limited to 35-yard range for performance
- Cluster analysis uses efficient O(n²) algorithm
- Memory-safe implementation with proper cleanup
- Configurable minimum thresholds to prevent unnecessary calculations

## Configuration Examples

### Maximum Smart Positioning
```ini
EnhancedGroundTargeting.SmartPositioning = 1
EnhancedGroundTargeting.MinEnemiesForSmart = 2
```

### Conservative Smart Positioning
```ini
EnhancedGroundTargeting.SmartPositioning = 1
EnhancedGroundTargeting.MinEnemiesForSmart = 3
```

### Disable Smart Positioning (Original Behavior)
```ini
EnhancedGroundTargeting.SmartPositioning = 0
```

## Troubleshooting

### Common Issues
1. **Spells not auto-targeting**: Ensure SQL file is applied and spell IDs are registered
2. **Smart positioning not working**: Check configuration settings and enemy count thresholds
3. **Performance issues**: Reduce scanning range or increase minimum enemy thresholds

### Debug Mode
Enable GM debug messages to see detailed information about positioning decisions and enemy detection.