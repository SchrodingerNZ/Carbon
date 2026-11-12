/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#pragma once

namespace Carbon
{

/** Immutable, game-defined achievement metadata. maxProgress == 0 denotes a boolean achievement. */
struct AchievementDefinition
{
    const char* id;
    const char* displayName;
    unsigned int maxProgress;
};

/** Registers metadata before engine initialization, including across the Carbon DLL boundary. */
class CARBON_API AchievementRegistration
{
public:
    explicit AchievementRegistration(const AchievementDefinition& definition);
};

/** First-pass in-memory implementation; no persistence or platform reporting yet. */
class CARBON_API AchievementManager
{
public:
    AchievementManager();
    ~AchievementManager();

    bool isReady() const;
    bool grant(const AchievementDefinition& achievement);
    bool setProgress(const AchievementDefinition& achievement, unsigned int progress);
    bool isGranted(const AchievementDefinition& achievement) const;
    unsigned int getProgress(const AchievementDefinition& achievement) const;

    /** Merges authoritative platform progress without decreasing locally observed progress. */
    bool mergeProgress(const AchievementDefinition& achievement, unsigned int progress);

private:
    class Members;
    Members* m;
};

}

// Stringified C++ symbol is the canonical platform achievement ID.
#define CARBON_ACHIEVEMENT_BOOLEAN(Symbol, DisplayName)                                \
    static const Carbon::AchievementDefinition Symbol = {#Symbol, DisplayName, 0};    \
    static const Carbon::AchievementRegistration Symbol##_registration(Symbol)

#define CARBON_ACHIEVEMENT_PROGRESS(Symbol, DisplayName, Maximum)                       \
    static const Carbon::AchievementDefinition Symbol = {#Symbol, DisplayName, Maximum}; \
    static const Carbon::AchievementRegistration Symbol##_registration(Symbol)

#define CARBON_ACHIEVEMENT_SELECT(_1, _2, _3, NAME, ...) NAME
#define ACHIEVEMENT(...) CARBON_ACHIEVEMENT_SELECT(__VA_ARGS__, CARBON_ACHIEVEMENT_PROGRESS, \
                                                  CARBON_ACHIEVEMENT_BOOLEAN)(__VA_ARGS__)
