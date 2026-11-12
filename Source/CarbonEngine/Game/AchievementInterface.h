/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

#include "CarbonEngine/Core/InterfaceRegistry.h"

namespace Carbon
{

/** Platform-specific achievement storage/reporting. A backend may initialise asynchronously. */
class CARBON_API AchievementInterface : private Noncopyable
{
public:
    virtual ~AchievementInterface() {}

    /** Return false to let the registry try the next implementation. */
    virtual bool isAvailable() const { return true; }

    /** Readiness is distinct from availability: future backends may load state asynchronously. */
    virtual bool isReady() const { return true; }

    /** Absolute, monotonically increasing progress. The manager validates and clamps input. */
    virtual bool setProgress(const String& id, unsigned int progress) = 0;
    virtual unsigned int getProgress(const String& id) const = 0;
};

CARBON_DECLARE_INTERFACE_REGISTRY(AchievementInterface);

}
