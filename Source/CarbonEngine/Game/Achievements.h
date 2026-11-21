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
    /** Canonical achievement identifier used by the connection platform. */
    const char* id;
    /** Game-defined human-readable achievement name. */
    const char* displayName;
    /** Progress required to grant the achievement; zero denotes a boolean achievement. */
    unsigned int maxProgress;
};

/** Registers metadata before engine initialization, including across the Carbon DLL boundary. */
class CARBON_API AchievementRegistration
{
public:
    /** Registers the supplied definition with the engine. */
    explicit AchievementRegistration(const AchievementDefinition& definition);
};

/** Game-facing achievement manager backed by the selected connection platform. */
class CARBON_API AchievementManager
{
public:
    AchievementManager();
    ~AchievementManager();

    /** Returns whether the selected achievement backend is ready. */
    bool isReady() const;
    /** Requests completion of the achievement; returns whether the backend accepted the update. */
    bool grant(const AchievementDefinition& achievement);
    /** Submits absolute progress, clamped to the achievement range; returns whether the update was accepted. */
    bool setProgress(const AchievementDefinition& achievement, unsigned int progress);
    /** Returns whether the registered achievement has reached its completion threshold. */
    bool isGranted(const AchievementDefinition& achievement) const;
    /** Returns current backend progress, or zero for an invalid definition or absent backend. */
    unsigned int getProgress(const AchievementDefinition& achievement) const;

    /** Merges authoritative platform progress without decreasing locally observed progress. */
    bool mergeProgress(const AchievementDefinition& achievement, unsigned int progress);

private:
    class Members;
    Members* m;
};

}

// Stringified C++ symbol is the canonical platform achievement ID.
// Omitting Maximum value-initializes maxProgress to zero for boolean achievements.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++98-compat-pedantic"
#endif
#define ACHIEVEMENT(Symbol, ...)                                                       \
    static const Carbon::AchievementDefinition Symbol = {#Symbol, __VA_ARGS__};       \
    static const Carbon::AchievementRegistration Symbol##_registration(Symbol)

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
