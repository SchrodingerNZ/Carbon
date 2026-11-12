/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "CarbonEngine/Common.h"
#include "CarbonEngine/Game/AchievementInterface.h"

namespace Carbon
{

CARBON_DEFINE_INTERFACE_REGISTRY(AchievementInterface)
{
    return i->isAvailable();
}

/** Non-persistent fallback; each engine initialization starts with empty progress. */
class NullAchievements : public AchievementInterface
{
public:
    bool setProgress(const String& id, unsigned int progress) override
    {
        auto& current = progress_[id.cStr()];
        if (progress > current)
            current = progress;
        return true;
    }

    unsigned int getProgress(const String& id) const override
    {
        const auto it = progress_.find(id.cStr());
        return it == progress_.end() ? 0 : it->second;
    }

private:
    std::unordered_map<std::string, unsigned int> progress_;
};

CARBON_REGISTER_INTERFACE_IMPLEMENTATION(AchievementInterface, NullAchievements, 0)

}
