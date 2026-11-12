/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Core/EventHandler.h"
#include "CarbonEngine/Core/InterfaceRegistry.h"
#include "CarbonEngine/Game/AchievementInterface.h"

namespace Carbon
{

/** The single selected external gaming connection; capabilities share its lifetime. */
class CARBON_API ConnectionPlatformInterface : public EventHandler, private Noncopyable
{
public:
    virtual ~ConnectionPlatformInterface() {}

    /** Synchronous selection check. Authentication and state loading may continue asynchronously. */
    virtual bool isAvailable() const { return true; }

    /** Achievement capability owned by this connection platform. */
    virtual AchievementInterface& getAchievements() = 0;

    /** Process external service callbacks once per engine update. */
    virtual void update() {}

    bool processEvent(const Event& e) override;
};

CARBON_DECLARE_INTERFACE_REGISTRY(ConnectionPlatformInterface);

}

// The client chooses an implementation at startup; without an override Null is selected.
// Example: CARBON_USE_CONNECTION_PLATFORM(Steam) (once Steam support is implemented).
#define CARBON_USE_CONNECTION_PLATFORM(Name) \
    CARBON_USE_INTERFACE_IMPLEMENTATION(Carbon::ConnectionPlatformInterface, Name##ConnectionPlatform)
