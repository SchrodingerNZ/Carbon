/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "CarbonEngine/Common.h"
#include "CarbonEngine/Game/Achievements.h"
#include "CarbonEngine/Game/AchievementInterface.h"
#include "CarbonEngine/Core/InterfaceRegistry.h"

namespace Carbon
{
namespace
{
struct RegisteredAchievement
{
    std::string displayName;
    unsigned int maxProgress;
};

// Intentionally process-lifetime: registration also occurs in static constructors/destructors.
std::unordered_map<std::string, RegisteredAchievement>& definitions()
{
    static auto* value = new std::unordered_map<std::string, RegisteredAchievement>;
    return *value;
}

bool valid(const AchievementDefinition& achievement)
{
    if (!achievement.id || !*achievement.id || !achievement.displayName)
        return false;
    const auto it = definitions().find(achievement.id);
    return it != definitions().end() && it->second.maxProgress == achievement.maxProgress &&
           it->second.displayName == achievement.displayName;
}
}

AchievementRegistration::AchievementRegistration(const AchievementDefinition& definition)
{
    if (!definition.id || !*definition.id || !definition.displayName)
        return;
    const auto result = definitions().emplace(definition.id,
                                               RegisteredAchievement{definition.displayName, definition.maxProgress});
    if (!result.second && (result.first->second.displayName != definition.displayName ||
                           result.first->second.maxProgress != definition.maxProgress))
        return; // Conflicting definitions are rejected by valid() at use time.
}

class AchievementManager::Members
{
public:
    AchievementInterface* backend = nullptr;
};

AchievementManager::AchievementManager() : m(new Members)
{
    m->backend = InterfaceRegistry<AchievementInterface>::create();
}

AchievementManager::~AchievementManager()
{
    InterfaceRegistry<AchievementInterface>::destroy();
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
    return m->backend->setProgress(achievement.id, progress);
}

bool AchievementManager::mergeProgress(const AchievementDefinition& achievement, unsigned int progress)
{
    return setProgress(achievement, progress);
}

unsigned int AchievementManager::getProgress(const AchievementDefinition& achievement) const
{
    if (!valid(achievement) || !m->backend)
        return 0;
    return m->backend->getProgress(achievement.id);
}

bool AchievementManager::isGranted(const AchievementDefinition& achievement) const
{
    if (!valid(achievement))
        return false;
    const auto progress = getProgress(achievement);
    return achievement.maxProgress ? progress >= achievement.maxProgress : progress != 0;
}
}
