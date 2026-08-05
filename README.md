# Enhanced Ground Targeting

Enhanced Ground Targeting automatically places supported destination-targeted spells at the selected target or at an
enemy cluster. It is opt-in per character and remains disabled until that character uses `.toggle on`.

## Behavior

- Character state defaults to off and is stored in the existing `character_settings` table.
- `.toggle`, `.toggle on`, `.toggle off`, and `.toggle status` control the feature.
- Smart placement uses each spell's calculated radius and range.
- Candidate destinations must be in range, on valid ground, and in line of sight.
- Combat-only mode excludes nearby enemies that are neither selected nor engaged with the player's party or pets.
- Playerbots remain disabled and do not perform persistence queries for this module.

## Supported spells

- Warlock: Rain of Fire and Summon Infernal
- Mage: Blizzard and Flamestrike
- Priest: Mass Dispel
- Druid: Hurricane
- Hunter: Volley and the custom trap-launcher spells
- Death Knight: Death and Decay
- Rogue: Distract

Damaging area spells use cluster placement. Single-target traps and utility spells use the selected target position.

## Configuration

- `EnhancedGroundTargeting.Enable`: globally enables the module.
- `EnhancedGroundTargeting.AutoTarget`: enables automatic destination replacement.
- `EnhancedGroundTargeting.CombatOnly`: includes only the selected enemy or enemies engaged with the player's party.
- `EnhancedGroundTargeting.SmartPositioning`: enables enemy-cluster placement for damaging area spells.
- `EnhancedGroundTargeting.MinEnemiesForSmart`: minimum covered enemies required to use a cluster destination.

Global enablement does not opt characters in. Each character must issue `.toggle on` once; the choice is then restored
on later logins and after server restarts.

## Installation

Configure and build AzerothCore with the module present, install it, and restart the worldserver. SQL files under
`data/sql/world/updates/` are discovered by AzerothCore's module database updater and applied automatically at startup
when world database updates are enabled.

The current SQL update removes legacy `spell_script_names` bindings because targeting is now handled by one early
module hook with an internal destination-spell registry.
