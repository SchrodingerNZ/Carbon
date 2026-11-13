/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "CarbonEngine/Common.h"
#include "CarbonEngine/Game/Achievements.h"
#include "CarbonEngine/Game/AchievementInterface.h"
#include "CarbonEngine/Game/ConnectionPlatformInterface.h"

namespace Carbon
{
namespace
{
struct RegisteredAchievement
{
    std::string displayName;
    unsigned int maxProgress;
    bool conflicted = false;
};

// Keep registration state in the Carbon DLL, with ordinary process-exit cleanup.
// AchievementRegistration has no destructor that accesses this registry.
std::unordered_map<std::string, RegisteredAchievement>& definitions()
{
    static std::unordered_map<std::string, RegisteredAchievement> value;
    return value;
}

bool valid(const AchievementDefinition& achievement)
{
    if (!achievement.id || !*achievement.id || !achievement.displayName)
        return false;
    const auto& registry = definitions();
    const auto it = registry.find(achievement.id);

    // A conflicting declaration invalidates the canonical ID for all callers.
    return it != registry.end() && !it->second.conflicted &&
           it->second.maxProgress == achievement.maxProgress &&
           it->second.displayName == achievement.displayName;
}
}

AchievementRegistration::AchievementRegistration(const AchievementDefinition& definition)
{
    if (!definition.id || !*definition.id || !definition.displayName)
        return;
    auto& registry = definitions();
    const auto result = registry.emplace(definition.id, RegisteredAchievement{definition.displayName,
                                                                             definition.maxProgress});

    // Identical declarations are harmless; conflicting metadata poisons the shared ID.
    if (!result.second && (result.first->second.displayName != definition.displayName ||
                           result.first->second.maxProgress != definition.maxProgress))
        result.first->second.conflicted = true;
}

class AchievementManager::Members
{
public:
    AchievementInterface* backend = nullptr;
};

AchievementManager::AchievementManager() : m(new Members)
{
    // The active connection platform owns the achievement capability.
    auto connection = InterfaceRegistry<ConnectionPlatformInterface>::getActiveInstance();
    m->backend = connection ? &connection->getAchievements() : nullptr;
}

AchievementManager::~AchievementManager()
{
    // The connection platform owns the backend and outlives this manager.
    delete m;
}

bool AchievementManager::isReady() const
{
    return m->backend && m->backend->isReady();
}

bool AchievementManager::grant(const AchievementDefinition& achievement)
{
    if (!valid(achievement))
        return false;
    return setProgress(achievement, achievement.maxProgress ? achievement.maxProgress : 1);
}

bool AchievementManager::setProgress(const AchievementDefinition& achievement, unsigned int progress)
{
    if (!valid(achievement) || !m->backend)
        return false;
    if (!achievement.maxProgress && progress > 0)
        progress = 1;
    if (achievement.maxProgress && progress > achievement.maxProgress)
        progress = achievement.maxProgress;
    return m->backend->setProgress(achievement.id, progress, achievement.maxProgress);
}

bool AchievementManager::mergeProgress(const AchievementDefinition& achievement, unsigned int progress)
{
    return setProgress(achievement, progress);
}

unsigned int AchievementManager::getProgress(const AchievementDefinition& achievement) const
{
    if (!valid(achievement) || !m->backend)
        return 0;
    return m->backend->getProgress(achievement.id, achievement.maxProgress);
}

bool AchievementManager::isGranted(const AchievementDefinition& achievement) const
{
    if (!valid(achievement))
        return false;
    const auto progress = getProgress(achievement);
    return achievement.maxProgress ? progress >= achievement.maxProgress : progress != 0;
}
}
