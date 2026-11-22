/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "CarbonEngine/Common.h"

#ifdef APPLE

#include "CarbonEngine/Game/ConnectionPlatformInterface.h"

namespace Carbon
{
namespace
{

// The skeleton does not accept achievement requests until reporting is implemented.
class GameKitAchievements : public AchievementInterface
{
public:
    bool isReady() const override { return false; }
    bool setProgress(const String&, unsigned int, unsigned int) override { return false; }
    unsigned int getProgress(const String&, unsigned int) const override { return 0; }
};

class GameKitMultiplayer : public MultiplayerInterface {};

class GameKitConnectionPlatform : public ConnectionPlatformInterface
{
public:
    AchievementInterface& getAchievements() override { return achievements_; }
    MultiplayerInterface& getMultiplayer() override { return multiplayer_; }

private:
    GameKitAchievements achievements_;
    GameKitMultiplayer multiplayer_;
};

using GameKitRegistry = InterfaceRegistry<ConnectionPlatformInterface>;

class GameKitFactory : public GameKitRegistry::Implementation
{
public:
    GameKitFactory() : GameKitRegistry::Implementation("GameKitConnectionPlatform", 0)
    {
        GameKitRegistry::registerImplementation(this);
    }

    ~GameKitFactory() override { GameKitRegistry::unregisterImplementation(this); }
    ConnectionPlatformInterface* create() override { return new GameKitConnectionPlatform; }
    void destroy(ConnectionPlatformInterface* instance) override { delete instance; }
};

} // namespace

// Called before registry selection. Explicit reference ensures the object is linked even
// when Carbon is a static archive. Do not register GameKit for Steam or default builds.
void registerGameKitConnectionPlatform()
{
    if (strcmp(GameKitRegistry::OverrideImplementationName, "GameKitConnectionPlatform") != 0)
        return;

    static GameKitFactory factory;
}

} // namespace Carbon

#endif
