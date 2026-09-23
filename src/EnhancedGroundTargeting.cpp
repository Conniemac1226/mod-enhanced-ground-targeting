#include "CellImpl.h"
#include "CharacterDatabase.h"
#include "Chat.h"
#include "Config.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerSettings.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <list>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr char SETTING_SOURCE[] = "mod_enhanced_ground_targeting";
constexpr uint32 SETTING_ENABLED = 0;
constexpr float DEFAULT_AOE_RADIUS = 8.0f;
constexpr float MAX_VERTICAL_SEPARATION = 6.0f;
constexpr float POSITION_EPSILON = 0.01f;

struct ModuleSettings
{
    bool Enabled = true;
    bool AutoTarget = true;
    bool CombatOnly = true;
    bool SmartPositioning = true;
    uint32 MinEnemiesForSmart = 2;
};

struct ToggleState : public DataMap::Base
{
    std::atomic<uint64> LoadId{0};
    std::atomic<bool> Enabled{false};
    std::atomic<bool> ChangedAfterLogin{false};
};

struct PlacementResult
{
    Position Destination;
    uint32 TargetCount = 0;
    bool IsValid = false;
};

struct PlacementCoverage
{
    uint32 TargetCount = 0;
    float Clearance = 0.0f;
};

ModuleSettings moduleSettings;
std::atomic<uint64> nextLoadId{0};

constexpr std::array<uint32, 48> SUPPORTED_SPELLS = {
    10, 1122, 1510, 1725, 2120, 2121, 5740, 6141, 6219, 8422, 8423, 8427, 10185, 10186, 10187, 10215,
    10216, 11677, 11678, 14294, 14295, 16914, 17401, 17402, 27012, 27022, 27085, 27086, 27212, 32375,
    42925, 42926, 42939, 42940, 43265, 47819, 47820, 48466, 49936, 49937, 49938, 58431, 58432, 900000,
    900001, 900002, 900003, 900004
};

bool IsSupportedSpell(uint32 spellId)
{
    return std::binary_search(SUPPORTED_SPELLS.begin(), SUPPORTED_SPELLS.end(), spellId);
}

bool UsesClusterPlacement(uint32 spellId)
{
    switch (spellId)
    {
        case 1122:   // Summon Infernal
        case 1725:   // Distract
        case 32375:  // Mass Dispel
        case 900000: // Launch Freezing Trap
        case 900002: // Launch Immolation Trap
            return false;
        default:
            return true;
    }
}

bool IsEnabledFor(Player const* player)
{
    ToggleState const* state = player->CustomData.Get<ToggleState>(SETTING_SOURCE);
    return state && state->Enabled.load(std::memory_order_relaxed);
}

void SaveToggleState(Player* player, bool enabled)
{
    ToggleState* state = player->CustomData.GetDefault<ToggleState>(SETTING_SOURCE);
    state->Enabled.store(enabled, std::memory_order_relaxed);
    state->ChangedAfterLogin.store(true, std::memory_order_relaxed);

    player->UpdatePlayerSetting(SETTING_SOURCE, SETTING_ENABLED, enabled ? 1 : 0);

    PlayerSettingVector settings;
    settings.emplace_back(enabled ? 1 : 0);
    CharacterDatabasePreparedStatement* stmt = PlayerSettingsStore::PrepareReplaceStatement(
        player->GetGUID().GetCounter(), SETTING_SOURCE, settings);
    CharacterDatabase.Execute(stmt);
}

void LoadToggleState(ObjectGuid guid, uint64 loadId, PreparedQueryResult result)
{
    Player* player = ObjectAccessor::FindConnectedPlayer(guid);
    ToggleState* state = player ? player->CustomData.Get<ToggleState>(SETTING_SOURCE) : nullptr;
    if (!state || state->LoadId.load(std::memory_order_relaxed) != loadId ||
        state->ChangedAfterLogin.load(std::memory_order_relaxed))
        return;

    bool enabled = false;
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            if (fields[0].Get<std::string>() != SETTING_SOURCE)
                continue;

            PlayerSettingVector settings = PlayerSettingsStore::ParseSettingsData(fields[1].Get<std::string>());
            enabled = !settings.empty() && settings[SETTING_ENABLED].IsEnabled();
            break;
        } while (result->NextRow());
    }

    state->Enabled.store(enabled, std::memory_order_relaxed);
}

std::vector<Unit*> CollectProtectedUnits(Player* player)
{
    std::vector<Unit*> protectedUnits;
    protectedUnits.push_back(player);
    if (Pet* pet = player->GetPet(); pet && pet->IsInMap(player))
        protectedUnits.push_back(pet);

    Group* group = player->GetGroup();
    if (!group)
        return protectedUnits;

    for (GroupReference* reference = group->GetFirstMember(); reference != nullptr; reference = reference->next())
    {
        Player* member = reference->GetSource();
        if (!member || member == player || !member->IsInMap(player))
            continue;

        protectedUnits.push_back(member);
        if (Pet* pet = member->GetPet(); pet && pet->IsInMap(player))
            protectedUnits.push_back(pet);
    }

    return protectedUnits;
}

bool IsCombatRelevant(Player* player, Unit* unit, std::vector<Unit*> const& protectedUnits)
{
    if (unit == player->GetSelectedUnit())
        return true;

    Unit* victim = unit->GetVictim();
    for (Unit* protectedUnit : protectedUnits)
        if (victim == protectedUnit || unit->IsInCombatWith(protectedUnit))
            return true;

    return false;
}

float GetAoeRadius(Spell* spell)
{
    float radius = 0.0f;
    SpellInfo const* spellInfo = spell->GetSpellInfo();
    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        if (!effect.IsEffect())
            continue;

        float const effectRadius = effect.CalcRadius(spell->GetCaster(), spell);
        if (std::isfinite(effectRadius) && effectRadius > radius)
            radius = effectRadius;
    }

    return radius > 0.0f ? radius : DEFAULT_AOE_RADIUS;
}

float GetSpellRange(Spell* spell)
{
    SpellInfo const* spellInfo = spell->GetSpellInfo();
    return spellInfo->GetMaxRange(spellInfo->IsPositive(), spell->GetCaster(), spell);
}

bool ValidateDestination(Player* player, Spell* spell, Position& destination)
{
    float x = destination.GetPositionX();
    float y = destination.GetPositionY();
    float z = destination.GetPositionZ();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;

    player->UpdateAllowedPositionZ(x, y, z);
    destination.Relocate(x, y, z, player->GetOrientation());
    if (!destination.IsPositionValid())
        return false;

    float const maxRange = GetSpellRange(spell);
    float const distance = player->GetExactDist(x, y, z);
    if (maxRange > 0.0f && distance > maxRange + player->GetLeewayBonusRadius())
        return false;

    float const minRange = spell->GetSpellInfo()->GetMinRange(spell->GetSpellInfo()->IsPositive());
    if (minRange > 0.0f && distance < minRange)
        return false;

    SpellInfo const* spellInfo = spell->GetSpellInfo();
    if (!spellInfo->HasAttribute(SPELL_ATTR2_IGNORE_LINE_OF_SIGHT) &&
        !spellInfo->HasAttribute(SPELL_ATTR5_ALWAYS_AOE_LINE_OF_SIGHT) &&
        !player->IsWithinLOS(x, y, z, VMAP::ModelIgnoreFlags::M2))
        return false;

    return true;
}

std::vector<Unit*> CollectTargets(Player* player, Spell* spell, float radius)
{
    float const maxRange = GetSpellRange(spell);
    float const scanRange = std::max(radius, maxRange + radius);
    std::vector<Unit*> const protectedUnits = CollectProtectedUnits(player);

    std::list<Unit*> nearbyUnits;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, scanRange);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearbyUnits, check);
    Cell::VisitObjects(player, searcher, scanRange);

    std::vector<Unit*> targets;
    targets.reserve(nearbyUnits.size());
    for (Unit* unit : nearbyUnits)
    {
        if (!unit || !unit->IsAlive() || unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            continue;

        if (moduleSettings.CombatOnly && !IsCombatRelevant(player, unit, protectedUnits))
            continue;

        if (!player->IsWithinLOSInMap(unit, VMAP::ModelIgnoreFlags::M2))
            continue;

        targets.push_back(unit);
    }

    return targets;
}

bool CoversTarget(Position const& destination, Unit const* unit, float radius)
{
    float const xDifference = destination.GetPositionX() - unit->GetPositionX();
    float const yDifference = destination.GetPositionY() - unit->GetPositionY();
    float const zDifference = std::fabs(destination.GetPositionZ() - unit->GetPositionZ());
    return xDifference * xDifference + yDifference * yDifference <= radius * radius &&
           zDifference <= MAX_VERTICAL_SEPARATION;
}

PlacementCoverage EvaluatePlacement(Position const& destination, std::vector<Unit*> const& targets, float radius)
{
    PlacementCoverage coverage;
    float farthestDistanceSquared = 0.0f;
    for (Unit const* unit : targets)
    {
        float const xDifference = destination.GetPositionX() - unit->GetPositionX();
        float const yDifference = destination.GetPositionY() - unit->GetPositionY();
        float const distanceSquared = xDifference * xDifference + yDifference * yDifference;
        if (distanceSquared > radius * radius ||
            std::fabs(destination.GetPositionZ() - unit->GetPositionZ()) > MAX_VERTICAL_SEPARATION)
            continue;

        ++coverage.TargetCount;
        farthestDistanceSquared = std::max(farthestDistanceSquared, distanceSquared);
    }

    coverage.Clearance = radius - std::sqrt(farthestDistanceSquared);
    return coverage;
}

void AddCircleIntersectionCandidates(std::vector<Position>& candidates, Unit const* first, Unit const* second,
                                     float radius)
{
    float const xDifference = second->GetPositionX() - first->GetPositionX();
    float const yDifference = second->GetPositionY() - first->GetPositionY();
    float const distanceSquared = xDifference * xDifference + yDifference * yDifference;
    if (distanceSquared <= POSITION_EPSILON || distanceSquared > 4.0f * radius * radius)
        return;

    float const distance = std::sqrt(distanceSquared);
    float const midpointX = (first->GetPositionX() + second->GetPositionX()) * 0.5f;
    float const midpointY = (first->GetPositionY() + second->GetPositionY()) * 0.5f;
    float const midpointZ = (first->GetPositionZ() + second->GetPositionZ()) * 0.5f;
    float const offsetLength = std::sqrt(std::max(0.0f, radius * radius - distanceSquared * 0.25f));
    float const offsetX = -yDifference * offsetLength / distance;
    float const offsetY = xDifference * offsetLength / distance;

    candidates.emplace_back(midpointX, midpointY, midpointZ);
    candidates.emplace_back(midpointX + offsetX, midpointY + offsetY, midpointZ);
    candidates.emplace_back(midpointX - offsetX, midpointY - offsetY, midpointZ);
}

void AddCoveredCentroidCandidates(std::vector<Position>& candidates, std::vector<Unit*> const& targets, float radius)
{
    size_t const initialCandidateCount = targets.size();
    for (size_t index = 0; index < initialCandidateCount; ++index)
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint32 count = 0;
        for (Unit const* unit : targets)
        {
            if (!CoversTarget(candidates[index], unit, radius))
                continue;

            x += unit->GetPositionX();
            y += unit->GetPositionY();
            z += unit->GetPositionZ();
            ++count;
        }

        if (count > 1)
            candidates.emplace_back(x / count, y / count, z / count);
    }
}

PlacementResult FindOptimalPlacement(Player* player, Spell* spell, std::vector<Unit*> const& targets, float radius)
{
    PlacementResult best;
    if (targets.size() < moduleSettings.MinEnemiesForSmart)
        return best;

    std::vector<Position> candidates;
    candidates.reserve(targets.size() * targets.size() * 2);
    for (Unit const* unit : targets)
        candidates.emplace_back(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ());

    AddCoveredCentroidCandidates(candidates, targets, radius);

    for (size_t first = 0; first < targets.size(); ++first)
        for (size_t second = first + 1; second < targets.size(); ++second)
            AddCircleIntersectionCandidates(candidates, targets[first], targets[second], radius);

    Unit* selectedTarget = player->GetSelectedUnit();
    bool bestCoversSelected = false;
    float bestClearance = -std::numeric_limits<float>::max();
    float bestSelectedDistance = std::numeric_limits<float>::max();
    float bestCasterDistance = std::numeric_limits<float>::max();

    for (Position candidate : candidates)
    {
        if (!ValidateDestination(player, spell, candidate))
            continue;

        PlacementCoverage const coverage = EvaluatePlacement(candidate, targets, radius);
        uint32 const targetCount = coverage.TargetCount;
        bool const coversSelected = selectedTarget && CoversTarget(candidate, selectedTarget, radius);
        float const clearance = coverage.Clearance;
        float const selectedDistance = selectedTarget ? selectedTarget->GetExactDist2d(
            candidate.GetPositionX(), candidate.GetPositionY()) : 0.0f;
        float const casterDistance = player->GetExactDist2d(candidate.GetPositionX(), candidate.GetPositionY());

        bool const isBetter = !best.IsValid || targetCount > best.TargetCount ||
            (targetCount == best.TargetCount && coversSelected && !bestCoversSelected) ||
            (targetCount == best.TargetCount && coversSelected == bestCoversSelected &&
             clearance > bestClearance + POSITION_EPSILON) ||
            (targetCount == best.TargetCount && coversSelected == bestCoversSelected &&
             std::fabs(clearance - bestClearance) <= POSITION_EPSILON && selectedDistance < bestSelectedDistance) ||
            (targetCount == best.TargetCount && coversSelected == bestCoversSelected &&
             std::fabs(clearance - bestClearance) <= POSITION_EPSILON &&
             std::fabs(selectedDistance - bestSelectedDistance) <= POSITION_EPSILON &&
             casterDistance < bestCasterDistance);
        if (!isBetter)
            continue;

        best.Destination = candidate;
        best.TargetCount = targetCount;
        best.IsValid = true;
        bestCoversSelected = coversSelected;
        bestClearance = clearance;
        bestSelectedDistance = selectedDistance;
        bestCasterDistance = casterDistance;
    }

    if (best.TargetCount < moduleSettings.MinEnemiesForSmart)
        return PlacementResult();

    return best;
}

bool SelectDestination(Player* player, Spell* spell, Position& destination)
{
    uint32 const spellId = spell->GetSpellInfo()->Id;
    if (moduleSettings.SmartPositioning && UsesClusterPlacement(spellId))
    {
        float const radius = GetAoeRadius(spell);
        std::vector<Unit*> targets = CollectTargets(player, spell, radius);
        PlacementResult optimal = FindOptimalPlacement(player, spell, targets, radius);
        if (optimal.IsValid)
        {
            destination = optimal.Destination;
            return true;
        }
    }

    if (Unit* selectedTarget = player->GetSelectedUnit())
        destination.Relocate(selectedTarget->GetPositionX(), selectedTarget->GetPositionY(),
                             selectedTarget->GetPositionZ(), player->GetOrientation());
    else
        destination.Relocate(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                             player->GetOrientation());

    return ValidateDestination(player, spell, destination);
}

void ApplyDestination(Spell* spell, Position const& destination)
{
    spell->m_targets.SetDst(destination);
    uint32 targetMask = spell->m_targets.GetTargetMask() | TARGET_FLAG_DEST_LOCATION;
    targetMask &= ~TARGET_FLAG_UNIT;
    targetMask &= ~TARGET_FLAG_GAMEOBJECT;
    spell->m_targets.SetTargetMask(targetMask);
    spell->m_targets.SetUnitTarget(nullptr);
}
}

class EnhancedGroundTargetingWorldScript : public WorldScript
{
public:
    EnhancedGroundTargetingWorldScript() : WorldScript("EnhancedGroundTargetingWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        moduleSettings.Enabled = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.Enable", true);
        moduleSettings.AutoTarget = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.AutoTarget", true);
        moduleSettings.CombatOnly = sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.CombatOnly", true);
        moduleSettings.SmartPositioning =
            sConfigMgr->GetOption<bool>("EnhancedGroundTargeting.SmartPositioning", true);
        moduleSettings.MinEnemiesForSmart = std::max<uint32>(
            1, sConfigMgr->GetOption<uint32>("EnhancedGroundTargeting.MinEnemiesForSmart", 2));

        LOG_INFO("server.loading",
                 "Enhanced Ground Targeting: enabled={}, autoTarget={}, combatOnly={}, smartPositioning={}, "
                 "minEnemies={}",
                 moduleSettings.Enabled, moduleSettings.AutoTarget, moduleSettings.CombatOnly,
                 moduleSettings.SmartPositioning, moduleSettings.MinEnemiesForSmart);
    }
};

class EnhancedGroundTargetingAllSpellScript : public AllSpellScript
{
public:
    EnhancedGroundTargetingAllSpellScript() : AllSpellScript("EnhancedGroundTargetingAllSpellScript") { }

    bool CanPrepare(Spell* spell, SpellCastTargets const* /*targets*/, AuraEffect const* /*triggeredByAura*/) override
    {
        WorldObject* casterObject = spell ? spell->GetCaster() : nullptr;
        Unit* caster = casterObject ? casterObject->ToUnit() : nullptr;
        Player* player = caster ? caster->ToPlayer() : nullptr;
        SpellInfo const* spellInfo = spell ? spell->GetSpellInfo() : nullptr;
        if (!player || !spellInfo || !IsSupportedSpell(spellInfo->Id))
            return true;

        if (!moduleSettings.Enabled || !moduleSettings.AutoTarget || !IsEnabledFor(player))
            return true;

        Position destination;
        if (SelectDestination(player, spell, destination))
            ApplyDestination(spell, destination);

        return true;
    }
};

class EnhancedGroundTargetingPlayerScript : public PlayerScript
{
public:
    EnhancedGroundTargetingPlayerScript() : PlayerScript("EnhancedGroundTargetingPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        if (!session || session->IsBot())
            return;

        ObjectGuid const guid = player->GetGUID();
        uint64 const loadId = ++nextLoadId;
        ToggleState* state = player->CustomData.GetDefault<ToggleState>(SETTING_SOURCE);
        state->LoadId.store(loadId, std::memory_order_relaxed);
        state->Enabled.store(false, std::memory_order_relaxed);
        state->ChangedAfterLogin.store(false, std::memory_order_relaxed);

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHAR_SETTINGS);
        stmt->SetData(0, guid.GetCounter());
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(stmt).WithPreparedCallback(
            [guid, loadId](PreparedQueryResult result)
            {
                LoadToggleState(guid, loadId, std::move(result));
            }));
    }

};

using namespace Acore::ChatCommands;

class EnhancedGroundTargetingCommandScript : public CommandScript
{
public:
    EnhancedGroundTargetingCommandScript() : CommandScript("EnhancedGroundTargetingCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "toggle", HandleToggleCommand, SEC_PLAYER, Console::No }
        };
        return commandTable;
    }

    static bool HandleToggleCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        if (!moduleSettings.Enabled)
        {
            handler->SendSysMessage("Enhanced Ground Targeting is disabled on this server.");
            return true;
        }

        std::string argument = args ? args : "";
        std::transform(argument.begin(), argument.end(), argument.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });

        bool enabled = IsEnabledFor(player);
        if (argument.empty())
            enabled = !enabled;
        else if (argument == "on" || argument == "enable" || argument == "1")
            enabled = true;
        else if (argument == "off" || argument == "disable" || argument == "0")
            enabled = false;
        else if (argument == "status")
        {
            handler->PSendSysMessage("Enhanced Ground Targeting is {}.", enabled ? "enabled" : "disabled");
            return true;
        }
        else
        {
            handler->SendSysMessage("Usage: .toggle [on|off|status]");
            return true;
        }

        SaveToggleState(player, enabled);
        handler->PSendSysMessage("Enhanced Ground Targeting: {}", enabled ? "|cff00ff00ENABLED|r" :
                                                                   "|cffff0000DISABLED|r");
        return true;
    }
};

void AddSC_EnhancedGroundTargeting()
{
    new EnhancedGroundTargetingWorldScript();
    new EnhancedGroundTargetingAllSpellScript();
    new EnhancedGroundTargetingPlayerScript();
    new EnhancedGroundTargetingCommandScript();
}
