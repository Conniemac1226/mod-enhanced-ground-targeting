#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "ObjectMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Unit.h"
#include "GameObject.h"
#include "World.h"
#include "Pet.h"
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include <vector>
#include <map>
#include <list>
#include <cmath>

// Player-specific toggle storage
static std::unordered_map<uint64, bool> playerToggleState;
static std::mutex toggleMutex;

// Helper functions for player toggle state
bool GetPlayerToggleState(uint64 playerGuid)
{
    std::lock_guard<std::mutex> lock(toggleMutex);
    auto it = playerToggleState.find(playerGuid);
    return it != playerToggleState.end() ? it->second : false; // Default to disabled
}

void SetPlayerToggleState(uint64 playerGuid, bool enabled)
{
    std::lock_guard<std::mutex> lock(toggleMutex);
    playerToggleState[playerGuid] = enabled;
}

// Structure to hold AOE position data
struct AOEPosition
{
    float x, y, z;
    uint32 targetCount;
    bool isValid;
    
    AOEPosition() : x(0.0f), y(0.0f), z(0.0f), targetCount(0), isValid(false) {}
    AOEPosition(float _x, float _y, float _z, uint32 _count) : x(_x), y(_y), z(_z), targetCount(_count), isValid(true) {}
};

// AzerothCore-style position validation (based on SpellEffects.cpp research)
bool ValidateAndAdjustPosition(Player* player, float& x, float& y, float& z, SpellInfo const* spellInfo)
{
    if (!player || !spellInfo)
        return false;
    
    float originalX = x, originalY = y, originalZ = z;
    float maxRange = spellInfo->GetMaxRange(false);
    
    // Phase 1: AzerothCore 6-yard Z-difference rule (SpellEffects.cpp:2502-2503)
    if (std::fabs(player->GetPositionZ() - z) > 6.0f)
    {
        z = player->GetPositionZ(); // Adjust Z like AzerothCore does
    }
    
    // Update ground position using AzerothCore method
    player->UpdateAllowedPositionZ(x, y, z);
    
    // Check basic range constraint
    float distanceToPlayer = player->GetExactDist2d(x, y);
    if (maxRange > 0 && distanceToPlayer <= maxRange)
    {
        return true; // Position valid - AzerothCore style (no LoS check)
    }
    
    // Phase 2: Use AzerothCore's GetRandomPoint method (SpellEffects.cpp:2467, 6049)
    float searchRadius = maxRange > 0 ? std::min(8.0f, maxRange * 0.3f) : 8.0f;
    Position originalPos(originalX, originalY, originalZ);
    Position randomPos = player->GetRandomPoint(originalPos, searchRadius);
    
    // AzerothCore automatically handles ground height in GetRandomPoint
    x = randomPos.GetPositionX();
    y = randomPos.GetPositionY(); 
    z = randomPos.GetPositionZ();
    
    // Check range constraint for random position
    distanceToPlayer = player->GetExactDist2d(x, y);
    if (maxRange <= 0 || distanceToPlayer <= maxRange)
    {
        return true; // Random position valid
    }
    
    // Phase 3: No valid position found - let spell fail naturally
    return false;
}

// Find maximum density cluster of enemies (based on playerbot algorithm)
std::vector<Unit*> FindMaxDensity(Player* player, float aoeRadius = 8.0f)
{
    std::vector<Unit*> allTargets;
    std::vector<Unit*> bestCluster;
    
    // Find all possible targets within reasonable range
    std::list<Unit*> targets;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck u_check(player, player, 35.0f);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, targets, u_check);
    Cell::VisitAllObjects(player, searcher, 35.0f);
    
    // Convert to vector and filter for combat-relevant targets only
    for (Unit* unit : targets)
    {
        if (!unit || !unit->IsAlive() || unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            continue;
            
        // Only include targets that are:
        // 1. Already in combat with the player
        // 2. The player's current target
        // 3. Currently attacking the player or player's pet
        bool isValidTarget = false;
        
        // Check if unit is in combat with player
        if (unit->IsInCombatWith(player))
        {
            isValidTarget = true;
        }
        // Check if unit is the current target
        else if (player->GetSelectedUnit() && player->GetSelectedUnit()->GetGUID() == unit->GetGUID())
        {
            isValidTarget = true;
        }
        // Check if unit is attacking player or player's pet
        else if (unit->GetVictim() && 
                (unit->GetVictim()->GetGUID() == player->GetGUID() || 
                 (player->GetPet() && unit->GetVictim()->GetGUID() == player->GetPet()->GetGUID())))
        {
            isValidTarget = true;
        }
        
        if (isValidTarget)
        {
            allTargets.push_back(unit);
        }
    }
    
    if (allTargets.empty())
        return bestCluster;
    
    std::map<Unit*, std::vector<Unit*>> groups;
    uint32 maxCount = 0;
    Unit* bestCenter = nullptr;
    
    // For each potential target, count how many other targets are within AOE radius
    for (Unit* unit : allTargets)
    {
        for (Unit* other : allTargets)
        {
            float distance = unit->GetExactDist2d(other);
            if (distance <= aoeRadius * 2.0f) // Use 2x radius for better clustering
            {
                groups[unit].push_back(other);
            }
        }
        
        if (groups[unit].size() > maxCount)
        {
            maxCount = groups[unit].size();
            bestCenter = unit;
        }
    }
    
    if (bestCenter && groups.find(bestCenter) != groups.end())
    {
        bestCluster = groups[bestCenter];
    }
    
    return bestCluster;
}

// Calculate optimal AOE position based on playerbot algorithm with validation
AOEPosition CalculateOptimalAOEPosition(Player* player, float aoeRadius = 8.0f, SpellInfo const* spellInfo = nullptr)
{
    std::vector<Unit*> cluster = FindMaxDensity(player, aoeRadius);
    
    if (cluster.empty())
        return AOEPosition();
    
    // Calculate bounding box of the cluster (playerbot method)
    float x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    bool firstUnit = true;
    
    for (Unit* unit : cluster)
    {
        if (!unit)
            continue;
            
        float unitX = unit->GetPositionX();
        float unitY = unit->GetPositionY();
        
        if (firstUnit)
        {
            x1 = x2 = unitX;
            y1 = y2 = unitY;
            firstUnit = false;
        }
        else
        {
            if (unitX < x1) x1 = unitX;
            if (unitX > x2) x2 = unitX;
            if (unitY < y1) y1 = unitY;
            if (unitY > y2) y2 = unitY;
        }
    }
    
    // Calculate center point of bounding box
    float centerX = (x1 + x2) / 2.0f;
    float centerY = (y1 + y2) / 2.0f;
    float centerZ = player->GetPositionZ();
    
    // Use enhanced validation system instead of basic UpdateAllowedPositionZ
    if (spellInfo && ValidateAndAdjustPosition(player, centerX, centerY, centerZ, spellInfo))
    {
        return AOEPosition(centerX, centerY, centerZ, cluster.size());
    }
    else
    {
        // Fallback: use basic method if enhanced validation fails
        player->UpdateAllowedPositionZ(centerX, centerY, centerZ);
        return AOEPosition(centerX, centerY, centerZ, cluster.size());
    }
}

// This is the spell script for auto-targeting ground AoE spells
class spell_enhanced_ground_targeting : public SpellScriptLoader
{
public:
    spell_enhanced_ground_targeting() : SpellScriptLoader("spell_enhanced_ground_targeting") {}

    class spell_enhanced_ground_targeting_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_enhanced_ground_targeting_SpellScript);

        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return true;
        }

        void HandleBeforeCast()
        {
            if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.AutoTarget", true))
                return;

            Unit* caster = GetCaster();
            if (!caster || !caster->ToPlayer())
                return;

            Player* player = caster->ToPlayer();
            
            // Check if player has toggled off the feature
            if (!GetPlayerToggleState(player->GetGUID().GetCounter()))
                return;
                
            Spell* spell = GetSpell();
            if (!spell)
                return;
                
            
                
            // Get spell info for AOE radius calculation
            SpellInfo const* spellInfo = GetSpellInfo();
            if (!spellInfo)
                return;
                
            // Determine AOE radius based on spell (default to 8.0f for most spells)
            float aoeRadius = 8.0f;
            uint32 spellId = spellInfo->Id;
            
            // Specific radius adjustments for known spells
            switch (spellId)
            {
                case 1510:  // Volley (Rank 1)
                case 14294: // Volley (Rank 2)
                case 14295: // Volley (Rank 3)
                case 27022: // Volley (Rank 4)
                case 58431: // Volley (Rank 5)
                case 58432: // Volley (Rank 6)
                    aoeRadius = 8.0f;
                    break;
                case 42208: // Blizzard (all ranks)
                case 42209:
                case 42210:
                case 42211:
                case 42212:
                case 42213:
                case 42214:
                case 42215:
                    aoeRadius = 8.0f;
                    break;
                case 5740:  // Rain of Fire (all ranks)
                case 6219:
                case 11677:
                case 11678:
                case 27212:
                case 47819:
                case 47820:
                    aoeRadius = 8.0f;
                    break;
                case 43265: // Death and Decay
                    aoeRadius = 8.0f;
                    break;
                default:
                    aoeRadius = 8.0f; // Default radius for unknown spells
                    break;
            }
            
            float targetX, targetY, targetZ;
            bool useOptimalPosition = false;
            
            // Check if smart positioning is enabled
            bool smartEnabled = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.SmartPositioning", true);
            uint32 minEnemies = sConfigMgr->GetOption<uint32>("EnhancedGroundTargeting.MinEnemiesForSmart", 2);
            
            if (smartEnabled)
            {
                // Try to calculate optimal AOE position
                AOEPosition optimalPos = CalculateOptimalAOEPosition(player, aoeRadius, spellInfo);
                
                if (optimalPos.isValid && optimalPos.targetCount >= minEnemies)
                {
                    // Use optimal position if we found a good cluster
                    targetX = optimalPos.x;
                    targetY = optimalPos.y;
                    targetZ = optimalPos.z;
                    useOptimalPosition = true;
                }
                else
                {
                    // Fallback to current target position
                    Unit* target = player->GetSelectedUnit();
                    if (target)
                    {
                        targetX = target->GetPositionX();
                        targetY = target->GetPositionY();
                        targetZ = target->GetPositionZ();
                        
                        ValidateAndAdjustPosition(player, targetX, targetY, targetZ, spellInfo);
                    }
                    else
                    {
                        targetX = player->GetPositionX();
                        targetY = player->GetPositionY();
                        targetZ = player->GetPositionZ();
                        
                        ValidateAndAdjustPosition(player, targetX, targetY, targetZ, spellInfo);
                    }
                }
            }
            else
            {
                Unit* target = player->GetSelectedUnit();
                if (target)
                {
                    targetX = target->GetPositionX();
                    targetY = target->GetPositionY();
                    targetZ = target->GetPositionZ();
                    
                    ValidateAndAdjustPosition(player, targetX, targetY, targetZ, spellInfo);
                }
                else
                {
                    targetX = player->GetPositionX();
                    targetY = player->GetPositionY();
                    targetZ = player->GetPositionZ();
                    
                    ValidateAndAdjustPosition(player, targetX, targetY, targetZ, spellInfo);
                }
            }
            // Set spell destination
            spell->m_targets.SetDst(targetX, targetY, targetZ, player->GetOrientation());
            
            uint32 targetFlags = spell->m_targets.GetTargetMask();
            targetFlags |= TARGET_FLAG_DEST_LOCATION;
            targetFlags &= ~TARGET_FLAG_UNIT;
            targetFlags &= ~TARGET_FLAG_GAMEOBJECT;
            spell->m_targets.SetTargetMask(targetFlags);
            
            spell->m_targets.SetUnitTarget(nullptr);
            spell->m_targets.SetSrc(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
            
        }

        void Register() override
        {
            OnCheckCast += SpellCheckCastFn(spell_enhanced_ground_targeting_SpellScript::HandleCheckCast);
            BeforeCast += SpellCastFn(spell_enhanced_ground_targeting_SpellScript::HandleBeforeCast);
        }
        
        SpellCastResult HandleCheckCast()
        {
            if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.AutoTarget", true))
                return SPELL_CAST_OK;

            Unit* caster = GetCaster();
            if (!caster || !caster->ToPlayer())
                return SPELL_CAST_OK;

            Player* player = caster->ToPlayer();
            
            // Check if player has toggled off the feature
            if (!GetPlayerToggleState(player->GetGUID().GetCounter()))
                return SPELL_CAST_OK;
                
            // Force a valid destination early to bypass cursor validation
            Spell* spell = GetSpell();
            if (!spell)
                return SPELL_CAST_OK;
                
            // Check if we already have a valid destination
            if (spell->m_targets.GetTargetMask() & TARGET_FLAG_DEST_LOCATION)
            {
                Position const* dest = spell->m_targets.GetDstPos();
                if (dest && dest->IsPositionValid())
                    return SPELL_CAST_OK; // Already has valid destination
            }
            
            // No valid destination, create one to prevent cursor errors
            Unit* target = player->GetSelectedUnit();
            float targetX, targetY, targetZ;
            
            if (target)
            {
                targetX = target->GetPositionX();
                targetY = target->GetPositionY();
                targetZ = target->GetPositionZ();
            }
            else
            {
                targetX = player->GetPositionX();
                targetY = player->GetPositionY();
                targetZ = player->GetPositionZ();
            }
            
            // Ensure valid ground position
            player->UpdateAllowedPositionZ(targetX, targetY, targetZ);
            
            // Set a temporary destination to prevent cursor validation errors
            spell->m_targets.SetDst(targetX, targetY, targetZ, player->GetOrientation());
            spell->m_targets.SetTargetMask(spell->m_targets.GetTargetMask() | TARGET_FLAG_DEST_LOCATION);
            
            return SPELL_CAST_OK;
        }
    };

    SpellScript* GetSpellScript() const override
    {
        return new spell_enhanced_ground_targeting_SpellScript();
    }
};

// Main class for the module
class EnhancedGroundTargeting : public WorldScript
{
public:
    EnhancedGroundTargeting() : WorldScript("EnhancedGroundTargeting") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        // Load configuration options
        enabled = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.Enable", true);
        autoTarget = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.AutoTarget", true);
        combatOnly = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.CombatOnly", true);
        smartPositioning = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.SmartPositioning", true);
        minEnemiesForSmart = sConfigMgr->GetOption<uint32>("EnhancedGroundTargeting.MinEnemiesForSmart", 2);

        if (enabled)
        {
            // Use proper logging for your core version
            LOG_INFO("server.loading", "Enhanced Ground Targeting Module: Enabled");
            if (autoTarget)
                LOG_INFO("server.loading", "Enhanced Ground Targeting Module: Auto-targeting enabled");
            if (combatOnly)
                LOG_INFO("server.loading", "Enhanced Ground Targeting Module: Combat-only targeting enabled");
            if (smartPositioning)
                LOG_INFO("server.loading", "Enhanced Ground Targeting Module: Smart positioning enabled (min enemies: {})", minEnemiesForSmart);
            
            LOG_INFO("server.loading", "Enhanced Ground Targeting: IMPORTANT: You need to apply the SQL to your database!");
        }
    }

private:
    bool enabled;
    bool autoTarget;
    bool combatOnly;
    bool smartPositioning;
    uint32 minEnemiesForSmart;
};

// All Spell Script for early interception
class EnhancedGroundTargeting_AllSpellScript : public AllSpellScript
{
public:
    EnhancedGroundTargeting_AllSpellScript() : AllSpellScript("EnhancedGroundTargeting_AllSpellScript") {}

    bool CanPrepare(Spell* spell, SpellCastTargets const* targets, AuraEffect const* /*triggeredByAura*/) override
    {
        if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.Enable", true))
            return true;
            
        if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.AutoTarget", true))
            return true;

        Unit* caster = spell->GetCaster();
        if (!caster || !caster->ToPlayer())
            return true;

        Player* player = caster->ToPlayer();
        
        // Check if player has toggled off the feature
        if (!GetPlayerToggleState(player->GetGUID().GetCounter()))
            return true;
            
        // Check if this is one of our registered spells
        uint32 spellId = spell->GetSpellInfo()->Id;
        bool isRegisteredSpell = false;
        
        // Check against registered spell IDs
        std::vector<uint32> registeredSpells = {
            1510, 14294, 14295, 27022, 58431, 58432, // Volley
            10, 6141, 8427, 10185, 10186, 10187, 27085, 42939, 42940, // Blizzard
            5740, 6219, 11677, 11678, 27212, 47819, 47820, // Rain of Fire
            43265, 49936, 49937, 49938, // Death and Decay
            2120, 2121, 8422, 8423, 10215, 10216, 27086, 42925, 42926 // Flamestrike
        };
        
        for (uint32 registeredId : registeredSpells)
        {
            if (spellId == registeredId)
            {
                isRegisteredSpell = true;
                break;
            }
        }
        
        if (!isRegisteredSpell)
            return true;
            
        // ALWAYS force a valid destination, regardless of current state
        Unit* target = player->GetSelectedUnit();
        float targetX, targetY, targetZ;
        
        if (target)
        {
            targetX = target->GetPositionX();
            targetY = target->GetPositionY();
            targetZ = target->GetPositionZ();
        }
        else
        {
            targetX = player->GetPositionX();
            targetY = player->GetPositionY();
            targetZ = player->GetPositionZ();
        }
        
        // Try smart positioning if enabled
        bool smartEnabled = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.SmartPositioning", true);
        if (smartEnabled && target)
        {
            AOEPosition optimalPos = CalculateOptimalAOEPosition(player, 8.0f, spell->GetSpellInfo());
            if (optimalPos.isValid && optimalPos.targetCount >= 2)
            {
                targetX = optimalPos.x;
                targetY = optimalPos.y;
                targetZ = optimalPos.z;
            }
        }
        
        ValidateAndAdjustPosition(player, targetX, targetY, targetZ, spell->GetSpellInfo());
        
        spell->m_targets.SetDst(targetX, targetY, targetZ, player->GetOrientation());
        spell->m_targets.SetTargetMask(TARGET_FLAG_DEST_LOCATION);
        spell->m_targets.SetUnitTarget(nullptr);
        
        return true;
    }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.Enable", true))
            return;
            
        if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.AutoTarget", true))
            return;

        Unit* caster = spell->GetCaster();
        if (!caster || !caster->ToPlayer())
            return;

        Player* player = caster->ToPlayer();
        
        // Check if player has toggled off the feature
        if (!GetPlayerToggleState(player->GetGUID().GetCounter()))
            return;
            
        // Check if this is one of our registered spells
        uint32 spellId = spell->GetSpellInfo()->Id;
        bool isRegisteredSpell = false;
        
        // Check against registered spell IDs
        std::vector<uint32> registeredSpells = {
            // Volley
            1510, 14294, 14295, 27022, 58431, 58432,
            // Blizzard
            10, 6141, 8427, 10185, 10186, 10187, 27085, 42939, 42940,
            // Rain of Fire
            5740, 6219, 11677, 11678, 27212, 47819, 47820,
            // Death and Decay
            43265, 49936, 49937, 49938,
            // Flamestrike
            2120, 2121, 8422, 8423, 10215, 10216, 27086, 42925, 42926
        };
        
        for (uint32 registeredId : registeredSpells)
        {
            if (spellId == registeredId)
            {
                isRegisteredSpell = true;
                break;
            }
        }
        
        if (!isRegisteredSpell)
            return;
            
        // Store original error before we start modifying things
        SpellCastResult originalError = res;
        
        // Check if we're getting a targeting error
        if (res == SPELL_FAILED_BAD_TARGETS || res == SPELL_FAILED_NO_VALID_TARGETS || 
            res == SPELL_FAILED_REQUIRES_AREA || res == SPELL_FAILED_BAD_IMPLICIT_TARGETS ||
            res == SPELL_FAILED_ONLY_OUTDOORS || res == SPELL_FAILED_LINE_OF_SIGHT ||
            res == SPELL_FAILED_OUT_OF_RANGE || res == SPELL_FAILED_TOO_CLOSE)
        {
            // Force a valid destination to bypass cursor validation
            Unit* target = player->GetSelectedUnit();
            float targetX, targetY, targetZ;
            
            if (target)
            {
                targetX = target->GetPositionX();
                targetY = target->GetPositionY();
                targetZ = target->GetPositionZ();
            }
            else
            {
                targetX = player->GetPositionX();
                targetY = player->GetPositionY();
                targetZ = player->GetPositionZ();
            }
            
            SpellInfo const* spellInfo = spell->GetSpellInfo();
            ValidateAndAdjustPosition(player, targetX, targetY, targetZ, spellInfo);
            
            spell->m_targets.SetDst(targetX, targetY, targetZ, player->GetOrientation());
            spell->m_targets.SetTargetMask(spell->m_targets.GetTargetMask() | TARGET_FLAG_DEST_LOCATION);
            
            // Handle specific error types
            if (originalError == SPELL_FAILED_ONLY_OUTDOORS)
            {
                targetX = player->GetPositionX() + 5.0f;
                targetY = player->GetPositionY() + 5.0f;
                targetZ = player->GetPositionZ();
                player->UpdateAllowedPositionZ(targetX, targetY, targetZ);
                spell->m_targets.SetDst(targetX, targetY, targetZ, player->GetOrientation());
            }
            
            res = SPELL_CAST_OK;
        }
        else if (!(spell->m_targets.GetTargetMask() & TARGET_FLAG_DEST_LOCATION))
        {
            Unit* target = player->GetSelectedUnit();
            float targetX, targetY, targetZ;
            
            if (target)
            {
                targetX = target->GetPositionX();
                targetY = target->GetPositionY();
                targetZ = target->GetPositionZ();
            }
            else
            {
                targetX = player->GetPositionX();
                targetY = player->GetPositionY();
                targetZ = player->GetPositionZ();
            }
            
            ValidateAndAdjustPosition(player, targetX, targetY, targetZ, spell->GetSpellInfo());
            
            spell->m_targets.SetDst(targetX, targetY, targetZ, player->GetOrientation());
            spell->m_targets.SetTargetMask(spell->m_targets.GetTargetMask() | TARGET_FLAG_DEST_LOCATION);
            
        }
    }
};

// Player Script for enhancing targeting
class EnhancedGroundTargeting_PlayerScript : public PlayerScript
{
public:
    EnhancedGroundTargeting_PlayerScript() : PlayerScript("EnhancedGroundTargeting_PlayerScript") {}

    void OnLogin(Player* player)
    {
        if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.Enable", true))
            return;

    }
    
    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
    }
};

// Command Script for .toggle command
using namespace Acore::ChatCommands;

class EnhancedGroundTargeting_CommandScript : public CommandScript
{
public:
    EnhancedGroundTargeting_CommandScript() : CommandScript("EnhancedGroundTargeting_CommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "toggle", HandleToggleCommand, SEC_PLAYER, Console::No },
            { "testcast", HandleTestCastCommand, SEC_PLAYER, Console::No }
        };
        return commandTable;
    }

    static bool HandleToggleCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.Enable", true))
        {
            handler->PSendSysMessage("Enhanced Ground Targeting is disabled on this server.");
            return true;
        }

        // Parse arguments
        std::string arg = args ? args : "";
        std::transform(arg.begin(), arg.end(), arg.begin(), ::tolower);

        uint64 playerGuid = player->GetGUID().GetCounter();
        bool currentState = GetPlayerToggleState(playerGuid);

        if (arg == "on" || arg == "enable" || arg == "1")
        {
            SetPlayerToggleState(playerGuid, true);
            handler->PSendSysMessage("Enhanced Ground Targeting: |cff00ff00ENABLED|r");
        }
        else if (arg == "off" || arg == "disable" || arg == "0")
        {
            SetPlayerToggleState(playerGuid, false);
            handler->PSendSysMessage("Enhanced Ground Targeting: |cffff0000DISABLED|r");
        }
        else
        {
            // Toggle current state
            bool newState = !currentState;
            SetPlayerToggleState(playerGuid, newState);
            handler->PSendSysMessage("Enhanced Ground Targeting: %s", 
                newState ? "|cff00ff00ENABLED|r" : "|cffff0000DISABLED|r");
        }

        return true;
    }
    
    static bool HandleTestCastCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        if (!sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.Enable", true))
        {
            handler->PSendSysMessage("Enhanced Ground Targeting is disabled on this server.");
            return true;
        }

        // Parse spell ID (default to Volley rank 1 if not specified)
        uint32 spellId = 1510; // Volley Rank 1
        if (args && strlen(args) > 0)
        {
            spellId = atoi(args);
        }
        
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
        {
            handler->PSendSysMessage("Invalid spell ID: %u", spellId);
            return true;
        }
        
        // Force enable the feature for this player temporarily
        bool wasEnabled = GetPlayerToggleState(player->GetGUID().GetCounter());
        SetPlayerToggleState(player->GetGUID().GetCounter(), true);
        
        // Create spell cast targets with forced destination
        Unit* target = player->GetSelectedUnit();
        float targetX, targetY, targetZ;
        
        if (target)
        {
            targetX = target->GetPositionX();
            targetY = target->GetPositionY();
            targetZ = target->GetPositionZ();
        }
        else
        {
            targetX = player->GetPositionX();
            targetY = player->GetPositionY();
            targetZ = player->GetPositionZ();
        }
        
        // Ensure valid ground position
        player->UpdateAllowedPositionZ(targetX, targetY, targetZ);
        
        // Try smart positioning if enabled
        bool smartEnabled = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.SmartPositioning", true);
        if (smartEnabled)
        {
            AOEPosition optimalPos = CalculateOptimalAOEPosition(player, 8.0f, spellInfo);
            if (optimalPos.isValid && optimalPos.targetCount >= 2)
            {
                targetX = optimalPos.x;
                targetY = optimalPos.y;
                targetZ = optimalPos.z;
            }
        }
        
        // Create spell cast targets
        SpellCastTargets targets;
        targets.SetDst(targetX, targetY, targetZ, player->GetOrientation());
        targets.SetTargetMask(TARGET_FLAG_DEST_LOCATION);
        
        // Cast the spell with forced validation bypass
        SpellCastResult result = player->CastSpell(targets, spellInfo, nullptr, TRIGGERED_FULL_MASK);
        
        // Restore original state
        SetPlayerToggleState(player->GetGUID().GetCounter(), wasEnabled);
        
        handler->PSendSysMessage("Test cast result: %s", result == SPELL_CAST_OK ? "SUCCESS" : "FAILED");
        
        return true;
    }
};

// AzerothCore script registration hook
void AddSC_EnhancedGroundTargeting()
{
    new EnhancedGroundTargeting();
    new spell_enhanced_ground_targeting();
    new EnhancedGroundTargeting_AllSpellScript();
    new EnhancedGroundTargeting_PlayerScript();
    new EnhancedGroundTargeting_CommandScript();
}