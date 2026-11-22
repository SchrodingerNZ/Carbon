/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "CarbonEngine/Common.h"

#ifdef APPLE

#include "CarbonEngine/Game/ConnectionPlatformInterface.h"

#undef new
#include <GameKit/GameKit.h>
#ifdef iOS
    #include <UIKit/UIKit.h>
#else
    #include <AppKit/AppKit.h>
#endif
#include <dispatch/dispatch.h>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include "CarbonEngine/Core/Memory/MemoryInterceptor.h"

namespace Carbon
{
namespace
{

#ifdef iOS
using AuthenticationViewController = UIViewController;
#else
using AuthenticationViewController = NSViewController;
#endif

// Only the main thread accesses this mailbox. Blocks hold a weak reference, never a provider pointer.
struct GameKitAuthenticationCallbacks
{
    AuthenticationViewController* pendingController = nil;
    NSError* error = nil;
    bool receivedResult = false;
};

class GameKitAuthentication
{
public:
    ~GameKitAuthentication()
    {
        // Invalidate callbacks before releasing the native authentication handler or UI.
        callbacks_.reset();
        if (installed_)
            [GKLocalPlayer localPlayer].authenticateHandler = nil;
        dismissAuthenticationUI();
    }

    void update()
    {
        // Carbon's application updates and native UI run on the Apple main thread.
        if (![NSThread isMainThread])
            return;

        auto player = [GKLocalPlayer localPlayer];
        if (!installed_)
        {
            installed_ = true;
            const std::weak_ptr<GameKitAuthenticationCallbacks> weakCallbacks(callbacks_);
            player.authenticateHandler = ^(AuthenticationViewController* controller, NSError* error) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    auto callbacks = weakCallbacks.lock();
                    if (!callbacks)
                        return;
                    callbacks->pendingController = controller;
                    callbacks->error = error;
                    callbacks->receivedResult = true;
                });
            };
        }

        if (callbacks_->receivedResult)
        {
            callbacks_->receivedResult = false;
            if (!callbacks_->pendingController)
            {
                dismissAuthenticationUI();
                if (callbacks_->error)
                    LOG_WARNING << "GameKit authentication: " << String(callbacks_->error.localizedDescription);
            }
            callbacks_->error = nil;
        }

        // Read the current account, not a potentially stale identity captured by an asynchronous callback.
        NSString* identifier = nil;
        if (player.authenticated)
        {
            if (@available(iOS 12.4, macOS 10.14.6, *))
                identifier = player.gamePlayerID;
            else
            {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                identifier = player.playerID;
#pragma clang diagnostic pop
            }
        }
        if (!identifier.length)
            identifier = nil;

        if (identifier != playerIdentifier_ && ![identifier isEqualToString:playerIdentifier_])
        {
            playerIdentifier_ = [identifier copy];
            ++generation_;
            LOG_INFO << (playerIdentifier_ ? "GameKit authenticated player changed" : "GameKit player signed out");
        }
        playerName_ = playerIdentifier_ ? String(player.displayName) : String("Player");

        if (playerIdentifier_)
        {
            callbacks_->pendingController = nil;
            dismissAuthenticationUI();
        }
        else if (callbacks_->pendingController && presentAuthenticationUI(callbacks_->pendingController))
            callbacks_->pendingController = nil;
    }

    bool isAuthenticated() const { return playerIdentifier_ != nil; }
    const String& getPlayerName() const { return playerName_; }
    uint64_t getGeneration() const { return generation_; }

private:
    bool presentAuthenticationUI(AuthenticationViewController* controller)
    {
        if (presentedController_ == controller)
            return true;
#ifdef iOS
        auto root = [UIApplication sharedApplication].keyWindow.rootViewController;
        if (!root || !root.view.window || root.presentedViewController)
            return false;
        [root presentViewController:controller animated:YES completion:nil];
#else
        auto parent = [NSApplication sharedApplication].mainWindow;
        if (!parent || parent.attachedSheet)
            return false;
        // Carbon's OpenGL window has a content view, not a content view controller.
        // Give authentication its own sheet rather than replacing the render view.
        authenticationWindow_ = [NSWindow windowWithContentViewController:controller];
        if (!authenticationWindow_)
            return false;
        [parent beginSheet:authenticationWindow_ completionHandler:nil];
#endif
        presentedController_ = controller;
        return true;
    }

    void dismissAuthenticationUI()
    {
#ifdef iOS
        if (presentedController_.presentingViewController)
            [presentedController_ dismissViewControllerAnimated:NO completion:nil];
#else
        if (authenticationWindow_)
        {
            auto parent = authenticationWindow_.sheetParent;
            if (parent)
                [parent endSheet:authenticationWindow_];
            [authenticationWindow_ orderOut:nil];
            authenticationWindow_ = nil;
        }
#endif
        presentedController_ = nil;
    }

    std::shared_ptr<GameKitAuthenticationCallbacks> callbacks_ = std::make_shared<GameKitAuthenticationCallbacks>();
    bool installed_ = false;
    NSString* playerIdentifier_ = nil;
    String playerName_ = "Player";
    uint64_t generation_ = 0;
    AuthenticationViewController* presentedController_ = nil;
#ifdef MACOS
    NSWindow* authenticationWindow_ = nil;
#endif
};

// A separate mailbox per load attempt prevents late results from a timed-out
// request or a previous account from replacing the current account's state.
struct GameKitAchievementLoad
{
    bool completed = false;
    bool succeeded = false;
    std::map<std::string, double> percentages;
};

struct GameKitAchievementReport
{
    bool completed = false;
    bool succeeded = false;
    std::map<std::string, double> percentages;
};

class GameKitAchievements : public AchievementInterface
{
public:
    explicit GameKitAchievements(const GameKitAuthentication& authentication) : authentication_(authentication) {}

    bool isReady() const override { return ready_ && authentication_.isAuthenticated() && generation_ == authentication_.getGeneration(); }

    bool setProgress(const String& id, unsigned int progress, unsigned int maxProgress) override
    {
        if (![NSThread isMainThread] || !*id.cStr())
            return false;
        synchronizeAccount(Clock::now());
        // Before the first authentication, accept startup requests for the initial account.
        // After sign-out, do not attribute new requests to an unknown subsequent account.
        if (!authentication_.isAuthenticated() && generation_ != 0)
            return false;
        const std::string key(id.cStr());
        if (![NSString stringWithUTF8String:key.c_str()])
            return false;
        const auto percent = maxProgress ? 100.0 * double(std::min(progress, maxProgress)) / double(maxProgress)
                                         : (progress ? 100.0 : 0.0);
        const auto known = percentages_.find(key);
        if (percent > 0.0 && (known == percentages_.end() || percent > known->second))
            requested_[key] = std::max(requested_[key], percent);
        return true;
    }

    unsigned int getProgress(const String& id, unsigned int maxProgress) const override
    {
        if (generation_ != authentication_.getGeneration())
            return 0;
        const auto it = percentages_.find(id.cStr());
        const auto pending = requested_.find(id.cStr());
        const auto percent = std::max(it == percentages_.end() ? 0.0 : it->second,
                                      pending == requested_.end() ? 0.0 : pending->second);
        if (!maxProgress)
            return percent >= 100.0 ? 1 : 0;
        if (percent >= 100.0)
            return maxProgress;
        // Recover the nearest integral progress without treating partial completion as an award.
        const auto progress = static_cast<unsigned int>(std::floor(percent * double(maxProgress) / 100.0 + 0.5));
        return std::min(progress, maxProgress - 1);
    }

    void update()
    {
        if (![NSThread isMainThread])
            return;
        const auto now = Clock::now();
        synchronizeAccount(now);
        if (!authentication_.isAuthenticated())
            return;
        if (ready_)
        {
            updateReporting(now);
            return;
        }

        if (load_ && load_->completed)
        {
            if (load_->succeeded)
            {
                percentages_.swap(load_->percentages);
                ready_ = true;
                LOG_INFO << "GameKit achievements loaded";
            }
            else
                LOG_WARNING << "GameKit achievement loading failed; will retry";
            load_.reset();
            nextLoad_ = now + std::chrono::seconds(5);
        }
        else if (load_ && now - loadStarted_ >= std::chrono::seconds(30))
        {
            LOG_WARNING << "GameKit achievement loading timed out; will retry";
            load_.reset();
            nextLoad_ = now + std::chrono::seconds(5);
        }

        if (ready_)
        {
            updateReporting(now);
            return;
        }
        if (load_ || now < nextLoad_)
            return;

        load_ = std::make_shared<GameKitAchievementLoad>();
        loadStarted_ = now;
        const std::weak_ptr<GameKitAchievementLoad> weakLoad(load_);
        [GKAchievement loadAchievementsWithCompletionHandler:^(NSArray<GKAchievement*>* achievements, NSError* error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                auto load = weakLoad.lock();
                if (!load)
                    return;
                load->succeeded = !error && achievements != nil;
                if (load->succeeded)
                {
                    for (GKAchievement* achievement in achievements)
                    {
                        const auto identifier = achievement.identifier.UTF8String;
                        const auto percent = achievement.percentComplete;
                        if (!identifier || !*identifier || !std::isfinite(percent))
                            continue;
                        auto& value = load->percentages[identifier];
                        value = std::max(value, achievement.completed ? 100.0 : std::min(100.0, std::max(0.0, percent)));
                    }
                }
                load->completed = true;
            });
        }];
    }

private:
    using Clock = std::chrono::steady_clock;
    void synchronizeAccount(Clock::time_point now)
    {
        if (generation_ == authentication_.getGeneration())
            return;
        // Only initial startup requests may be transferred to the first authenticated account.
        if (generation_ != 0 || !authentication_.isAuthenticated())
        {
            if (!requested_.empty())
                LOG_WARNING << "GameKit discarded pending achievements on account change";
            requested_.clear();
        }
        generation_ = authentication_.getGeneration();
        ready_ = false;
        percentages_.clear();
        load_.reset();
        report_.reset();
        nextLoad_ = nextReport_ = now;
    }

    void updateReporting(Clock::time_point now)
    {
        if (report_ && report_->completed)
        {
            if (report_->succeeded)
            {
                for (const auto& entry : report_->percentages)
                    percentages_[entry.first] = std::max(percentages_[entry.first], entry.second);
            }
            else
                LOG_WARNING << "GameKit achievement reporting failed; will retry";
            report_.reset();
            nextReport_ = now + std::chrono::seconds(5);
        }
        else if (report_ && now - reportStarted_ >= std::chrono::seconds(60))
        {
            // Each attempt has its own mailbox. A late callback cannot acknowledge a newer attempt.
            // Game Center only increases reported progress, so retrying the same absolute value is safe.
            LOG_WARNING << "GameKit achievement reporting timed out; will retry";
            report_.reset();
            nextReport_ = now + std::chrono::seconds(5);
        }

        for (auto it = requested_.begin(); it != requested_.end();)
        {
            const auto known = percentages_.find(it->first);
            if (known != percentages_.end() && known->second >= it->second)
                it = requested_.erase(it);
            else
                ++it;
        }
        if (report_ || requested_.empty() || now < nextReport_)
            return;

        auto nativeAchievements = [NSMutableArray array];
        auto attempt = std::make_shared<GameKitAchievementReport>();
        for (const auto& entry : requested_)
        {
            auto identifier = [NSString stringWithUTF8String:entry.first.c_str()];
            auto achievement = [[GKAchievement alloc] initWithIdentifier:identifier];
            if (!achievement)
                continue;
            achievement.percentComplete = entry.second;
            [nativeAchievements addObject:achievement];
            attempt->percentages.emplace(entry.first, entry.second);
        }
        if (!nativeAchievements.count)
        {
            nextReport_ = now + std::chrono::seconds(5);
            LOG_WARNING << "GameKit could not construct pending achievements; will retry";
            return;
        }
        report_ = attempt;
        reportStarted_ = now;
        const std::weak_ptr<GameKitAchievementReport> weakReport(report_);
        [GKAchievement reportAchievements:nativeAchievements withCompletionHandler:^(NSError* error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                auto report = weakReport.lock();
                if (!report)
                    return;
                report->succeeded = error == nil;
                report->completed = true;
            });
        }];
    }

    const GameKitAuthentication& authentication_;
    uint64_t generation_ = 0;
    bool ready_ = false;
    std::map<std::string, double> percentages_;
    std::map<std::string, double> requested_;
    std::shared_ptr<GameKitAchievementReport> report_;
    Clock::time_point nextReport_ = Clock::now();
    Clock::time_point reportStarted_ = Clock::now();
    std::shared_ptr<GameKitAchievementLoad> load_;
    Clock::time_point nextLoad_ = Clock::now();
    Clock::time_point loadStarted_ = Clock::now();
};

// A native match is only a transport candidate. It does not become a Carbon
// lobby until role negotiation and network-version agreement have succeeded.
struct GameKitMatchmakingAttempt
{
    MultiplayerOperation operation = MultiplayerOperation::FindLobby;
    String networkVersion;
    unsigned int maxMembers = 0;
    GKMatch* match = nil;
    bool completed = false;
    bool failed = false;
};

class GameKitMatchmaking
{
public:
    explicit GameKitMatchmaking(const GameKitAuthentication& authentication) : authentication_(authentication) {}
    ~GameKitMatchmaking() { cancel(); }

    // min/max describe the native matching request, not a published Carbon roster.
    // The caller will choose these when the role negotiation policy is implemented.
    bool begin(MultiplayerOperation operation, const String& version, unsigned int minMembers, unsigned int maxMembers)
    {
        if (![NSThread isMainThread] || !authentication_.isAuthenticated() || attempt_ ||
            (operation != MultiplayerOperation::CreateLobby && operation != MultiplayerOperation::FindLobby) ||
            !*version.cStr() || minMembers < 2 || maxMembers < minMembers ||
            maxMembers > [GKMatchRequest maxPlayersAllowedForMatchOfType:GKMatchTypePeerToPeer])
            return false;

        auto request = [[GKMatchRequest alloc] init];
        request.minPlayers = minMembers;
        request.maxPlayers = maxMembers;
        // Stable, provider-private partition by protocol version. Negotiation must
        // still compare the full version string, since this hash can collide.
        uint32_t group = 2166136261u;
        for (auto p = version.cStr(); *p; ++p)
            group = (group ^ static_cast<unsigned char>(*p)) * 16777619u;
        request.playerGroup = group & 0x7fffffffu;

        attempt_ = std::make_shared<GameKitMatchmakingAttempt>();
        attempt_->operation = operation;
        attempt_->networkVersion = version;
        attempt_->maxMembers = maxMembers;
        generation_ = authentication_.getGeneration();
        const std::weak_ptr<GameKitMatchmakingAttempt> weakAttempt(attempt_);
        [[GKMatchmaker sharedMatchmaker] findMatchForRequest:request withCompletionHandler:^(GKMatch* match, NSError* error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                auto attempt = weakAttempt.lock();
                if (!attempt)
                {
                    // A cancelled request may still produce a native match.
                    [match disconnect];
                    return;
                }
                attempt->failed = error != nil || match == nil;
                if (attempt->failed)
                    [match disconnect];
                else
                    attempt->match = match;
                attempt->completed = true;
            });
        }];
        return true;
    }

    void cancel()
    {
        if (!attempt_)
            return;
        const auto pending = !attempt_->completed;
        auto match = attempt_->match;
        // Invalidate the mailbox before asking GameKit to cancel the operation.
        attempt_.reset();
        if (pending)
            [[GKMatchmaker sharedMatchmaker] cancel];
        [match disconnect];
    }

    void update()
    {
        if (attempt_ && (!authentication_.isAuthenticated() || generation_ != authentication_.getGeneration()))
            cancel();
    }

    bool isPending() const { return bool(attempt_); }

    // Transfer ownership of a completed native request to the negotiation stage.
    // That stage must install the match delegate and own subsequent disconnection.
    std::shared_ptr<GameKitMatchmakingAttempt> takeCompleted()
    {
        if (!attempt_ || !attempt_->completed)
            return nullptr;
        auto result = attempt_;
        attempt_.reset();
        return result;
    }

private:
    const GameKitAuthentication& authentication_;
    uint64_t generation_ = 0;
    std::shared_ptr<GameKitMatchmakingAttempt> attempt_;
};

class GameKitMultiplayer : public MultiplayerInterface
{
public:
    explicit GameKitMultiplayer(const GameKitAuthentication& authentication)
        : authentication_(authentication), matchmaking_(authentication) {}
    String getLocalPlayerName() const override { return authentication_.getPlayerName(); }

    void update() { matchmaking_.update(); }

    // Public matchmaking remains unavailable until the negotiation and transport
    // stages can establish a valid Carbon session and dispatch completion events.
private:
    const GameKitAuthentication& authentication_;
    GameKitMatchmaking matchmaking_;
};

class GameKitConnectionPlatform : public ConnectionPlatformInterface
{
public:
    GameKitConnectionPlatform() : achievements_(authentication_), multiplayer_(authentication_) {}

    void update() override
    {
        authentication_.update();
        achievements_.update();
        multiplayer_.update();
    }

    AchievementInterface& getAchievements() override { return achievements_; }
    MultiplayerInterface& getMultiplayer() override { return multiplayer_; }

private:
    GameKitAuthentication authentication_;
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
