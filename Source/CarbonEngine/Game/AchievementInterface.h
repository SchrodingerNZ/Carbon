/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once


namespace Carbon
{

/** Achievement storage/reporting supplied by the selected connection platform. */
class CARBON_API AchievementInterface : private Noncopyable
{
public:
    virtual ~AchievementInterface() {}

    /** Readiness is distinct from availability: future backends may load state asynchronously. */
    virtual bool isReady() const { return true; }

    /** Absolute, monotonically increasing progress. The manager validates and clamps input. */
    virtual bool setProgress(const String& id, unsigned int progress) = 0;
    virtual unsigned int getProgress(const String& id) const = 0;
};

}
