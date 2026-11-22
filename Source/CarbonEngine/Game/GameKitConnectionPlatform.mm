/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "CarbonEngine/Common.h"

#ifdef APPLE

#include "CarbonEngine/Game/ConnectionPlatformInterface.h"
#include "CarbonEngine/Game/SessionController.hpp"

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
#include <vector>
#include "CarbonEngine/Core/Memory/MemoryInterceptor.h"

// Native callbacks only enqueue data. The Carbon provider consumes it on update.
@interface CarbonGameKitMatchDelegate : NSObject<GKMatchDelegate>
@property(nonatomic, copy) void (^receive)(GKMatch*, NSData*, GKPlayer*);
@property(nonatomic, copy) void (^failure)(GKMatch*);
@end

@implementation CarbonGameKitMatchDelegate
- (void)match:(GKMatch*)match didReceiveData:(NSData*)data fromRemotePlayer:(GKPlayer*)player
{
    if (self.receive)
        self.receive(match, data, player);
}
- (void)match:(GKMatch*)match player:(GKPlayer*)player didChangeConnectionState:(GKPlayerConnectionState)state
{
    if (state == GKPlayerStateDisconnected && self.failure)
        self.failure(match);
}
- (void)match:(GKMatch*)match didFailWithError:(NSError*)error
{
    if (self.failure)
        self.failure(match);
}
@end

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
            !*version.cStr() || strlen(version.cStr()) > 256 || minMembers < 2 || maxMembers < minMembers || maxMembers > 32 ||
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

// Use the same identity convention for local and remote players. Never trust an
// identity claimed inside a packet; GameKit supplies the authenticated sender.
NSString* gameKitMemberIdentity(GKPlayer* player)
{
    if (@available(iOS 12.4, macOS 10.14.6, *))
        return player.gamePlayerID;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return player.playerID;
#pragma clang diagnostic pop
}

struct GameKitMatchPacket
{
    NSData* data = nil;
    GKPlayer* sender = nil;
};

struct GameKitMatchMailbox
{
    GKMatch* match = nil;
    std::vector<GameKitMatchPacket> packets;
    bool failed = false;
};

// Separate session traffic from JSON negotiation packets without interpreting
// untrusted payloads until the negotiated session has been adopted.
static const std::string gameKitSessionPrefix = "CGKS1|";

static bool isGameKitSessionPacket(NSData* data)
{
    return data.length >= gameKitSessionPrefix.size() &&
           memcmp(data.bytes, gameKitSessionPrefix.data(), gameKitSessionPrefix.size()) == 0;
}

// A private candidate uses the same registry and manifest machinery as Steam.
// It is not the application's public session until the transport is ready.
struct GameKitSessionCandidate
{
    SessionDetail::SessionController<std::string> controller;
    MultiplayerSession session;
    std::string manifest;

    bool initialize(uint64_t sessionID, const std::string& host, const std::string& local,
                    const std::map<std::string, std::string>& names)
    {
        if (session.getID() || !sessionID || host.empty() || local.empty() || names.size() < 2 || names.size() > 32 ||
            names.count(host) != 1 || names.count(local) != 1)
            return false;
        for (const auto& entry : names)
            if (entry.first.empty() || entry.second.empty())
                return false;

        auto& registry = controller.registry();
        // Every peer has agreed on the native roster. Allocate the host first
        // and the rest in canonical byte order (not localized display order).
        const PlayerID hostID(registry.assign(host));
        for (const auto& entry : names)
            registry.assign(entry.first);

        SessionDetail::SessionPlayerRegistry<std::string>::DestinationMap destinations;
        for (const auto& entry : names)
            destinations.emplace(registry.assign(entry.first), entry.first);
        Vector<MultiplayerPlayer> players;
        for (const auto& entry : destinations)
            players.append(MultiplayerPlayer(PlayerID(entry.first), String(names.at(entry.second).c_str()),
                                             MultiplayerConnectionState::Connected));
        const PlayerID localID(registry.assign(local));
        controller.enterLobby(session, sessionID);
        controller.reconcileRoster(session, sessionID, host == local, hostID, localID, players, destinations);

        using Codec = SessionDetail::SessionManifestCodec;
        const auto encodeIdentity = [](const std::string& member) { return Codec::encodeManifestName(member); };
        const auto getName = [&names](const std::string& member) { return names.at(member); };
        manifest = Codec::appendSessionMetadata(Codec::buildRoster(destinations, encodeIdentity, getName),
                                                MultiplayerSessionMetadata());
        MultiplayerSessionMetadata metadata;
        std::string reconstructed;
        return controller.validateManifest(session, manifest, encodeIdentity, getName, metadata, reconstructed) ==
               SessionDetail::SessionController<std::string>::ManifestValidation::Valid;
    }

    std::string agreementKey() const
    {
        return std::to_string(session.getID()) + "|" + manifest;
    }
};

// Own the native transport and its existing callback mailbox continuously across
// the negotiation/session boundary. No delegate replacement is needed.
struct GameKitActiveMatch
{
    std::shared_ptr<GameKitMatchmakingAttempt> attempt;
    std::shared_ptr<GameKitMatchMailbox> mailbox;
    std::shared_ptr<GameKitSessionCandidate> candidate;
    CarbonGameKitMatchDelegate* delegate = nil;
    uint64_t accountGeneration = 0;

    ~GameKitActiveMatch()
    {
        mailbox.reset();
        if (attempt)
        {
            attempt->match.delegate = nil;
            [attempt->match disconnect];
        }
    }
};

// A fixed native roster must agree on the full protocol version, exactly one
// creator, and the same host identity before it can become a Carbon session.
// Hello and acknowledgement packets carry the same immutable participant intent.
class GameKitNegotiation
{
public:
    explicit GameKitNegotiation(const GameKitAuthentication& authentication) : authentication_(authentication) {}
    ~GameKitNegotiation() { cancel(); }

    bool begin(const std::shared_ptr<GameKitMatchmakingAttempt>& attempt)
    {
        if (attempt_ || !attempt || !attempt->match || attempt->failed || !authentication_.isAuthenticated())
            return false;
        failureReason_ = MultiplayerOperationFailureReason::None;
        operation_ = attempt->operation;
        attempt_ = attempt;
        generation_ = authentication_.getGeneration();
        mailbox_ = std::make_shared<GameKitMatchMailbox>();
        mailbox_->match = attempt_->match;
        delegate_ = [[CarbonGameKitMatchDelegate alloc] init];
        const std::weak_ptr<GameKitMatchMailbox> weakMailbox(mailbox_);
        delegate_.receive = ^(GKMatch* match, NSData* data, GKPlayer* sender) {
            dispatch_async(dispatch_get_main_queue(), ^{
                auto mailbox = weakMailbox.lock();
                if (!mailbox || mailbox->match != match || mailbox->failed)
                    return;
                // Negotiation packets are small. Bound the mailbox so malformed
                // or excessive traffic cannot accumulate indefinitely.
                if (!data || !sender || data.length > 32768 || mailbox->packets.size() >= 256)
                {
                    mailbox->failed = true;
                    return;
                }
                GameKitMatchPacket packet;
                packet.data = data;
                packet.sender = sender;
                mailbox->packets.push_back(packet);
            });
        };
        delegate_.failure = ^(GKMatch* match) {
            dispatch_async(dispatch_get_main_queue(), ^{
                auto mailbox = weakMailbox.lock();
                if (mailbox && mailbox->match == match)
                    mailbox->failed = true;
            });
        };
        attempt_->match.delegate = delegate_;
        started_ = Clock::now();
        nextSend_ = started_;
        return true;
    }

    void cancel()
    {
        // Invalidate queued callbacks before disconnecting the native match.
        mailbox_.reset();
        if (attempt_)
        {
            attempt_->match.delegate = nil;
            [attempt_->match disconnect];
        }
        delegate_ = nil;
        attempt_.reset();
        roster_ = nil;
        localIdentity_ = nil;
        hostIdentity_ = nil;
        intents_.clear();
        acknowledgements_.clear();
        candidate_.reset();
        earlySessionPackets_.clear();
        ready_ = false;
        sentAcknowledgement_ = false;
        failureReason_ = MultiplayerOperationFailureReason::None;
    }

    bool isPending() const { return bool(attempt_) && !ready_; }
    bool isReady() const { return ready_; }
    MultiplayerOperation getOperation() const { return operation_; }
    MultiplayerOperationFailureReason getFailureReason() const { return failureReason_; }
    NSString* getHostIdentity() const { return hostIdentity_; }
    NSArray<NSString*>* getRoster() const { return roster_; }
    std::shared_ptr<GameKitSessionCandidate> getCandidate() const { return ready_ ? candidate_ : nullptr; }

    std::shared_ptr<GameKitActiveMatch> takeReady()
    {
        if (!ready_ || !candidate_ || !attempt_ || !mailbox_ || mailbox_->failed)
            return nullptr;
        auto active = std::make_shared<GameKitActiveMatch>();
        active->attempt.swap(attempt_);
        active->mailbox.swap(mailbox_);
        active->candidate.swap(candidate_);
        active->delegate = delegate_;
        active->accountGeneration = generation_;
        // Older buffered packets must precede packets queued since the last update.
        earlySessionPackets_.insert(earlySessionPackets_.end(), active->mailbox->packets.begin(),
                                    active->mailbox->packets.end());
        active->mailbox->packets.swap(earlySessionPackets_);
        // Clear negotiation state without disconnecting the transferred match.
        cancel();
        return active;
    }

    void update()
    {
        if (!attempt_)
            return;
        if (!authentication_.isAuthenticated() || generation_ != authentication_.getGeneration() || mailbox_->failed)
        {
            reject("account or native match changed");
            return;
        }
        // The next integration stage will consume the agreed match and mailbox.
        if (ready_)
            return;
        const auto now = Clock::now();
        if (now - started_ >= std::chrono::seconds(30))
        {
            reject("negotiation timed out");
            return;
        }
        if (attempt_->match.expectedPlayerCount != 0)
        {
            if (roster_)
                reject("native roster changed");
            return;
        }
        auto roster = readRoster();
        if (!roster || (roster_ && ![roster_ isEqualToArray:roster]))
        {
            reject("invalid or changed roster");
            return;
        }
        if (!roster_)
        {
            roster_ = roster;
            const bool creator = attempt_->operation == MultiplayerOperation::CreateLobby;
            NSString* name = [GKLocalPlayer localPlayer].displayName;
            if (!validString(name, 512))
                name = @"Player";
            // Only the creator supplies the session identifier. It is independent
            // of the native match object and changes on each creation attempt.
            uint64_t sessionID = 0;
            if (creator)
            {
                arc4random_buf(&sessionID, sizeof(sessionID));
                if (!sessionID)
                    sessionID = 1;
            }
            intents_.emplace(key(localIdentity_), Intent(creator, creator ? attempt_->maxMembers : 0,
                                                        key(name), sessionID));
        }
        std::vector<GameKitMatchPacket> packets;
        packets.swap(mailbox_->packets);
        for (const auto& packet : packets)
        {
            if (isGameKitSessionPacket(packet.data))
            {
                if (earlySessionPackets_.size() >= 256)
                {
                    reject("early session mailbox overflow");
                    return;
                }
                earlySessionPackets_.push_back(packet);
            }
            else if (!receive(packet))
            {
                reject("invalid or incompatible negotiation packet");
                return;
            }
        }

        if (intents_.size() == roster_.count && !hostIdentity_)
        {
            unsigned int creators = 0;
            for (const auto& entry : intents_)
                if (entry.second.creator)
                {
                    ++creators;
                    if (entry.second.capacity < roster_.count)
                    {
                        reject("creator capacity is smaller than the native roster");
                        return;
                    }
                    hostIdentity_ = [NSString stringWithUTF8String:entry.first.c_str()];
                }
            if (creators != 1)
            {
                reject("match requires exactly one creator");
                return;
            }
            if (!prepareCandidate())
            {
                reject("invalid Carbon session candidate");
                return;
            }
        }
        if (candidate_)
            for (const auto& acknowledgement : acknowledgements_)
                if (acknowledgement.second.first != key(hostIdentity_) ||
                    acknowledgement.second.second != candidate_->agreementKey())
                {
                    reject("participants disagree on Carbon session candidate");
                    return;
                }

        // Repeat until every participant has acknowledged the same roster and
        // host. A peer may have installed its delegate after our first hello.
        if (now >= nextSend_)
        {
            nextSend_ = now + std::chrono::seconds(1);
            if (sendIntent() && hostIdentity_)
                sentAcknowledgement_ = true;
        }
        if (candidate_ && sentAcknowledgement_ && acknowledgements_.size() + 1 == roster_.count)
        {
            // Every member has acknowledged the exact initial manifest. This is
            // lobby agreement only; Syncing will require fresh confirmations and
            // independent gameplay readiness via SessionController.
            auto& agreement = candidate_->controller.agreement();
            agreement.publish(1, candidate_->manifest, key(localIdentity_));
            for (const auto& entry : acknowledgements_)
                agreement.confirmMember(entry.first);
            agreement.markLocalConfirmation(1, MultiplayerSessionState::Lobby);
            ready_ = true;
            LOG_INFO << "GameKit match negotiation completed";
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    struct Intent
    {
        Intent(bool c, unsigned int n, const std::string& displayName, uint64_t id)
            : creator(c), capacity(n), name(displayName), sessionID(id) {}
        bool creator;
        unsigned int capacity;
        std::string name;
        uint64_t sessionID;
    };

    static std::string key(NSString* value) { return value.UTF8String ? value.UTF8String : ""; }
    static bool validString(id value, NSUInteger limit)
    {
        if (![value isKindOfClass:[NSString class]])
            return false;
        NSString* string = value;
        const auto bytes = string.UTF8String;
        const auto length = [string lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
        return bytes && length > 0 && length <= limit && strlen(bytes) == length;
    }

    NSArray<NSString*>* readRoster()
    {
        localIdentity_ = gameKitMemberIdentity([GKLocalPlayer localPlayer]);
        if (!validString(localIdentity_, 512))
            return nil;
        auto members = [NSMutableArray<NSString*> arrayWithObject:localIdentity_];
        for (GKPlayer* player in attempt_->match.players)
        {
            auto identity = gameKitMemberIdentity(player);
            if (!validString(identity, 512) || [members containsObject:identity])
                return nil;
            [members addObject:identity];
        }
        if (members.count < 2 || members.count > attempt_->maxMembers || members.count > 32)
            return nil;
        return [members sortedArrayUsingComparator:^NSComparisonResult(NSString* a, NSString* b) {
            return [a compare:b options:NSLiteralSearch];
        }];
    }

    bool prepareCandidate()
    {
        const auto host = key(hostIdentity_);
        const auto local = key(localIdentity_);
        const auto hostIntent = intents_.find(host);
        if (hostIntent == intents_.end() || !hostIntent->second.sessionID)
            return false;

        std::map<std::string, std::string> names;
        for (NSString* identity in roster_)
        {
            const auto member = key(identity);
            const auto found = intents_.find(member);
            if (found == intents_.end())
                return false;
            // Use Carbon's existing NSString-to-String conversion uniformly.
            // The manifest must contain exactly the same bytes as the snapshot.
            String name([NSString stringWithUTF8String:found->second.name.c_str()]);
            if (!name.length())
                name = "Player";
            if (!names.emplace(member, std::string(name.cStr(), name.length())).second)
                return false;
        }
        auto candidate = std::make_shared<GameKitSessionCandidate>();
        if (names.size() != roster_.count || !candidate->initialize(hostIntent->second.sessionID, host, local, names))
            return false;
        candidate_ = candidate;
        return true;
    }

    bool sendIntent()
    {
        const auto& intent = intents_.at(key(localIdentity_));
        auto version = [NSString stringWithUTF8String:attempt_->networkVersion.cStr()];
        if (!validString(version, 256))
            return false;
        auto name = [NSString stringWithUTF8String:intent.name.c_str()];
        auto sessionID = [NSString stringWithUTF8String:std::to_string(intent.sessionID).c_str()];
        auto agreement = candidate_ ? [NSString stringWithUTF8String:candidate_->agreementKey().c_str()] : @"";
        NSArray* message = @[@"CarbonGK2", version, @(intent.creator ? 1 : 0), @(intent.capacity), roster_,
                             hostIdentity_ ? hostIdentity_ : @"", name, sessionID, agreement];
        auto data = [NSJSONSerialization dataWithJSONObject:message options:0 error:nullptr];
        if (!data || data.length > 32768)
            return false;
        NSError* error = nil;
        return [attempt_->match sendDataToAllPlayers:data withDataMode:GKMatchSendDataReliable error:&error];
    }

    bool receive(const GameKitMatchPacket& packet)
    {
        auto sender = gameKitMemberIdentity(packet.sender);
        if (!validString(sender, 512) || [sender isEqualToString:localIdentity_])
            return false;
        // A peer may reach its complete roster before us. Only process packets
        // once the local native roster is complete, and compare the entire roster.
        if (![roster_ containsObject:sender])
            return false;
        id decoded = [NSJSONSerialization JSONObjectWithData:packet.data options:0 error:nullptr];
        if (![decoded isKindOfClass:[NSArray class]] || [(NSArray*)decoded count] != 9)
            return false;
        NSArray* fields = decoded;
        if (![@"CarbonGK2" isEqual:fields[0]] || !validString(fields[1], 256) ||
            ![fields[1] isEqualToString:[NSString stringWithUTF8String:attempt_->networkVersion.cStr()]] ||
            ![fields[2] isKindOfClass:[NSNumber class]] || ![fields[3] isKindOfClass:[NSNumber class]] ||
            ![fields[4] isKindOfClass:[NSArray class]] || ![roster_ isEqualToArray:fields[4]] ||
            ![fields[5] isKindOfClass:[NSString class]] || !validString(fields[6], 512) ||
            !validString(fields[7], 20) || ![fields[8] isKindOfClass:[NSString class]])
            return false;
        const double role = [fields[2] doubleValue];
        const double capacity = [fields[3] doubleValue];
        if ((role != 0 && role != 1) || (role == 0 && capacity != 0) ||
            (role == 1 && !(capacity >= 2 && capacity <= 32 && std::floor(capacity) == capacity)))
            return false;
        const auto suppliedID = key(fields[7]);
        char* end = nullptr;
        const auto sessionID = std::strtoull(suppliedID.c_str(), &end, 10);
        if (!end || *end || std::to_string(sessionID) != suppliedID || bool(sessionID) != (role == 1))
            return false;
        const Intent intent(role == 1, static_cast<unsigned int>(capacity), key(fields[6]), sessionID);
        const auto identity = key(sender);
        auto existing = intents_.find(identity);
        if (existing != intents_.end() && (existing->second.creator != intent.creator ||
            existing->second.capacity != intent.capacity || existing->second.name != intent.name ||
            existing->second.sessionID != intent.sessionID))
            return false;
        intents_.emplace(identity, intent);
        NSString* host = fields[5];
        NSString* agreement = fields[8];
        if (host.length)
        {
            if (![roster_ containsObject:host] || !validString(agreement, 32768))
                return false;
            const auto proposed = std::make_pair(key(host), key(agreement));
            auto previous = acknowledgements_.find(identity);
            if (previous != acknowledgements_.end() && previous->second != proposed)
                return false;
            acknowledgements_.emplace(identity, proposed);
        }
        else if (agreement.length)
            return false;
        return true;
    }

    void reject(const char* reason)
    {
        LOG_WARNING << "GameKit match rejected: " << reason;
        cancel();
        failureReason_ = MultiplayerOperationFailureReason::LobbyEntryFailed;
    }

    const GameKitAuthentication& authentication_;
    uint64_t generation_ = 0;
    std::shared_ptr<GameKitMatchmakingAttempt> attempt_;
    std::shared_ptr<GameKitMatchMailbox> mailbox_;
    CarbonGameKitMatchDelegate* delegate_ = nil;
    NSArray<NSString*>* roster_ = nil;
    NSString* localIdentity_ = nil;
    NSString* hostIdentity_ = nil;
    std::map<std::string, Intent> intents_;
    std::map<std::string, std::pair<std::string, std::string>> acknowledgements_;
    std::shared_ptr<GameKitSessionCandidate> candidate_;
    std::vector<GameKitMatchPacket> earlySessionPackets_;
    Clock::time_point started_ = Clock::now();
    Clock::time_point nextSend_ = Clock::now();
    bool ready_ = false;
    bool sentAcknowledgement_ = false;
    MultiplayerOperation operation_ = MultiplayerOperation::FindLobby;
    MultiplayerOperationFailureReason failureReason_ = MultiplayerOperationFailureReason::None;
};

class GameKitMultiplayer : public MultiplayerInterface
{
public:
    explicit GameKitMultiplayer(const GameKitAuthentication& authentication)
        : authentication_(authentication), matchmaking_(authentication), negotiation_(authentication) {}

    bool isAvailable() const override { return authentication_.isAuthenticated(); }
    String getLocalPlayerName() const override { return authentication_.getPlayerName(); }
    bool isInLobby() const override { return bool(active_); }
    bool isLobbyPending() const override { return pending_; }
    bool isSearching() const override { return pending_ && operation_ == MultiplayerOperation::FindLobby; }

    // GameKit has no externally joinable platform lobby identifier. The Carbon
    // session ID remains available through getSession(). Explicit join is inherited.
    bool findLobby(const String& version) override
    {
        if (![NSThread isMainThread])
            return false;
        const auto maximum = [GKMatchRequest maxPlayersAllowedForMatchOfType:GKMatchTypePeerToPeer];
        return begin(MultiplayerOperation::FindLobby, version, maximum > 32 ? 32u : static_cast<unsigned int>(maximum));
    }

    bool createLobby(const String& version, unsigned int maxMembers) override
    {
        return begin(MultiplayerOperation::CreateLobby, version, maxMembers);
    }

    void leaveLobby() override { endSession(MultiplayerSessionEndReason::Left); }

    bool setSessionMetadata(const String& key, const String& value) override
    {
        if (!canEditMetadata() || !Codec::validMetadataKey(key) || value.length() > 256)
            return false;
        auto& controller = active_->candidate->controller;
        auto metadata = controller.metadataDirty() ? controller.stagedMetadata() : getSession().getMetadata();
        metadata[key] = value;
        if (!Codec::validMetadata(metadata))
            return false;
        controller.stageMetadata(metadata);
        return true;
    }

    bool removeSessionMetadata(const String& key) override
    {
        if (!canEditMetadata() || !Codec::validMetadataKey(key))
            return false;
        auto& controller = active_->candidate->controller;
        auto metadata = controller.metadataDirty() ? controller.stagedMetadata() : getSession().getMetadata();
        if (!metadata.erase(key))
            return false;
        controller.stageMetadata(metadata);
        return true;
    }

    bool publishMetadata() override
    {
        if (!canEditMetadata())
            return false;
        auto& controller = active_->candidate->controller;
        if (!controller.metadataDirty())
            return true;
        const auto metadata = controller.stagedMetadata();
        const auto manifest = buildManifest(metadata);
        auto& agreement = controller.agreement();
        if (manifest != agreement.manifest())
        {
            const auto revision = agreement.revision() + 1;
            if (!revision || !sendControl(snapshot('L', revision, manifest)))
                return false;
            agreement.publish(revision, manifest, localMember());
        }
        controller.discardMetadata();
        if (getSession().getMetadata() != metadata)
        {
            mutableSession().setMetadata(metadata);
            dispatchSessionUpdated();
        }
        return true;
    }

    bool startGame() override
    {
        if (!canEditMetadata() || !nativeRosterIntact())
            return false;
        auto& controller = active_->candidate->controller;
        auto& agreement = controller.agreement();
        if (controller.metadataDirty() || !agreement.revision() ||
            agreement.manifest() != buildManifest(getSession().getMetadata()))
            return false;
        if (!sendControl(snapshot('S', agreement.revision(), agreement.manifest())))
            return false;
        enterSyncing();
        return true;
    }

    bool setReady(bool ready) override
    {
        if (!sessionUsable() || getSession().getState() != MultiplayerSessionState::Syncing)
            return false;
        auto& controller = active_->candidate->controller;
        auto& agreement = controller.agreement();
        if (isHost())
        {
            agreement.setMemberReady(localMember(), ready);
            beginPlayingIfReady();
        }
        else
        {
            agreement.requestLocalReadiness(ready);
            sendGuestAgreement();
        }
        return true;
    }

    unsigned int getConnectedPlayerCount() const override
    {
        unsigned int count = 0;
        for (unsigned int i = 0; active_ && i < getSession().getPlayerCount(); ++i)
        {
            const auto player = getSession().getPlayer(i);
            if (player && player->getID() != getSession().getLocalPlayerID() &&
                player->getConnectionState() == MultiplayerConnectionState::Connected)
                ++count;
        }
        return count;
    }

    PlayerID getConnectedPlayer(unsigned int index) const override
    {
        for (unsigned int i = 0; active_ && i < getSession().getPlayerCount(); ++i)
        {
            const auto player = getSession().getPlayer(i);
            if (player && player->getID() != getSession().getLocalPlayerID() &&
                player->getConnectionState() == MultiplayerConnectionState::Connected && index-- == 0)
                return player->getID();
        }
        return PlayerID();
    }

    void update()
    {
        if (active_)
        {
            if (!authentication_.isAuthenticated() || active_->accountGeneration != authentication_.getGeneration() ||
                active_->mailbox->failed)
            {
                endSession(MultiplayerSessionEndReason::Left);
                return;
            }
            updateSession();
            return;
        }
        if (!pending_)
            return;
        if (!authentication_.isAuthenticated() || accountGeneration_ != authentication_.getGeneration())
        {
            fail(MultiplayerOperationFailureReason::ServiceFailure);
            return;
        }
        matchmaking_.update();
        auto attempt = matchmaking_.takeCompleted();
        if (attempt)
        {
            if (attempt->failed)
            {
                fail(MultiplayerOperationFailureReason::ServiceFailure);
                return;
            }
            if (!negotiation_.begin(attempt))
            {
                [attempt->match disconnect];
                fail(MultiplayerOperationFailureReason::LobbyEntryFailed);
                return;
            }
        }
        negotiation_.update();
        if (negotiation_.getFailureReason() != MultiplayerOperationFailureReason::None && !matchmaking_.isPending())
        {
            fail(negotiation_.getFailureReason());
            return;
        }
        auto ready = negotiation_.takeReady();
        if (ready)
            adopt(ready);
    }

private:
    using Controller = SessionDetail::SessionController<std::string>;
    using Codec = SessionDetail::SessionManifestCodec;

    bool sessionUsable() const
    {
        return active_ && active_->mailbox && !active_->mailbox->failed && authentication_.isAuthenticated() &&
               active_->accountGeneration == authentication_.getGeneration();
    }

    bool isHost() const { return getSession().getHostPlayerID() == getSession().getLocalPlayerID(); }

    std::string localMember() const
    {
        return active_->candidate->controller.registry().destinations().at(getSession().getLocalPlayerID().getValue());
    }

    bool canEditMetadata() const
    {
        return sessionUsable() && isHost() && getSession().getState() == MultiplayerSessionState::Lobby;
    }

    std::string memberName(const std::string& member) const
    {
        const auto& assignments = active_->candidate->controller.registry().assignments();
        const auto found = assignments.find(member);
        if (found == assignments.end())
            return {};
        const auto player = getSession().getPlayer(PlayerID(found->second));
        return player ? std::string(player->getName().cStr(), player->getName().length()) : std::string();
    }

    std::string buildManifest(const MultiplayerSessionMetadata& metadata) const
    {
        return Codec::appendSessionMetadata(Codec::buildRoster(active_->candidate->controller.registry().destinations(),
            [](const std::string& member) { return Codec::encodeManifestName(member); },
            [this](const std::string& member) { return memberName(member); }), metadata);
    }

    bool nativeRosterIntact() const
    {
        if (!sessionUsable() || active_->attempt->match.expectedPlayerCount != 0)
            return false;
        const auto& members = active_->candidate->controller.registry().assignments();
        std::set<std::string> connected;
        connected.insert(localMember());
        for (GKPlayer* player in active_->attempt->match.players)
        {
            NSString* identity = gameKitMemberIdentity(player);
            if (!identity.UTF8String || !members.count(identity.UTF8String) || !connected.insert(identity.UTF8String).second)
                return false;
        }
        return connected.size() == members.size();
    }

    static std::string snapshot(char phase, uint64_t revision, const std::string& manifest)
    {
        return std::string(1, phase) + "|" + std::to_string(revision) + "|" + manifest;
    }

    std::string sessionPrefix() const { return gameKitSessionPrefix + std::to_string(getSession().getID()) + "|"; }

    bool sendControl(const std::string& message)
    {
        if (!sessionUsable())
            return false;
        const auto bytes = sessionPrefix() + message;
        if (bytes.size() > 32768)
            return false;
        auto data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        NSError* error = nil;
        if ([active_->attempt->match sendDataToAllPlayers:data withDataMode:GKMatchSendDataReliable error:&error])
            return true;
        // A failed broadcast may have reached a subset. Do not continue with
        // divergent authoritative state; terminate on the next update.
        active_->mailbox->failed = true;
        LOG_WARNING << "GameKit session control submission failed";
        return false;
    }

    void enterSyncing()
    {
        auto& controller = active_->candidate->controller;
        auto& frozen = controller.frozenMembers();
        frozen.clear();
        for (const auto& member : controller.registry().destinations())
            frozen.push_back(member.second);
        controller.beginSynchronization(localMember());
        const auto previous = getSession().getState();
        mutableSession().setIdentity(getSession().getID(), controller.state(), getSession().getHostPlayerID(),
                                     getSession().getLocalPlayerID());
        dispatchSessionStateChanged(previous);
        // Callers must revalidate active_ after this callback.
    }

    void beginPlayingIfReady()
    {
        if (!sessionUsable() || !isHost())
            return;
        auto& controller = active_->candidate->controller;
        if (!controller.canBeginPlaying(true) || !nativeRosterIntact())
            return;
        auto& agreement = controller.agreement();
        if (!sendControl(snapshot('P', agreement.revision(), agreement.manifest())))
            return;
        const auto previous = controller.enterPlaying(mutableSession(), getSession().getID());
        dispatchSessionStateChanged(previous);
    }

    void sendGuestAgreement()
    {
        if (!sessionUsable() || isHost())
            return;
        auto& controller = active_->candidate->controller;
        auto& agreement = controller.agreement();
        const auto phase = controller.state();
        if (phase != MultiplayerSessionState::Lobby && phase != MultiplayerSessionState::Syncing)
            return;
        const auto revision = agreement.revision();
        if (!agreement.hasLocalConfirmation(revision, phase))
        {
            if (!sendControl(controller.confirmationMessage(revision, agreement.manifest())))
                return;
            agreement.markLocalConfirmation(revision, phase);
        }
        if (agreement.hasPendingReadiness() && sendControl(controller.readinessMessage(revision)))
            agreement.markLocalReadinessSent(revision);
    }

    bool acceptSnapshot(const std::string& message)
    {
        auto& controller = active_->candidate->controller;
        auto& agreement = controller.agreement();
        const auto phase = message[0];
        uint64_t revision = 0;
        std::string manifest;
        if (!Codec::decodeSnapshot(message.substr(2), revision, manifest))
            return false;
        if (revision < agreement.revision())
            return true; // old reliable publication, never roll back
        MultiplayerSessionMetadata metadata;
        std::string reconstructed;
        if (controller.validateManifest(getSession(), manifest,
            [](const std::string& member) { return Codec::encodeManifestName(member); },
            [this](const std::string& member) { return memberName(member); }, metadata, reconstructed) !=
            Controller::ManifestValidation::Valid)
            return false;
        if (revision == agreement.revision() && manifest != agreement.manifest())
            return false;
        const auto state = controller.state();
        if (phase == 'L')
        {
            if (state != MultiplayerSessionState::Lobby)
                return revision == agreement.revision();
            if (revision > agreement.revision())
                agreement.publish(revision, manifest, localMember());
            if (metadata != getSession().getMetadata())
            {
                mutableSession().setMetadata(metadata);
                dispatchSessionUpdated();
            }
            return true;
        }
        if (revision != agreement.revision() || manifest != agreement.manifest())
            return false; // the locked snapshot must already have been published
        if (phase == 'S')
        {
            if (state == MultiplayerSessionState::Lobby)
                enterSyncing();
            return true;
        }
        if (state == MultiplayerSessionState::Playing)
            return true;
        if (state != MultiplayerSessionState::Syncing || !agreement.hasLocalConfirmation(revision, state) ||
            agreement.requestedReadiness() != 1 || agreement.hasPendingReadiness())
            return false;
        const auto previous = controller.enterPlaying(mutableSession(), getSession().getID());
        dispatchSessionStateChanged(previous);
        return true;
    }

    bool receiveControl(const GameKitMatchPacket& packet)
    {
        // Negotiation retransmissions may still be queued after adoption.
        if (!isGameKitSessionPacket(packet.data))
            return true;
        const std::string bytes(static_cast<const char*>(packet.data.bytes), packet.data.length);
        const auto prefix = sessionPrefix();
        if (bytes.compare(0, prefix.size(), prefix) != 0)
            return false;
        const auto message = bytes.substr(prefix.size());
        NSString* identity = gameKitMemberIdentity(packet.sender);
        if (!identity.UTF8String || message.empty())
            return false;
        const std::string sender(identity.UTF8String);
        auto& controller = active_->candidate->controller;
        const auto& assignments = controller.registry().assignments();
        const auto found = assignments.find(sender);
        if (found == assignments.end() || sender == localMember())
            return false;
        if (message.size() >= 2 && message[1] == '|' &&
            (message[0] == 'L' || message[0] == 'S' || message[0] == 'P'))
        {
            if (isHost() || PlayerID(found->second) != getSession().getHostPlayerID())
                return false;
            return acceptSnapshot(message);
        }
        // Guest confirmations and readiness are broadcast, but only the host
        // consumes them. Reliable per-sender ordering preserves their sequence.
        if (!isHost())
            return true;
        std::string value;
        if (controller.acceptReadinessMessage(sender, true, message, value))
            return true;
        uint64_t revision = 0;
        return controller.acceptConfirmationMessage(sender, true, message, revision) != Controller::ConfirmationResult::Invalid;
    }

    void updateSession()
    {
        const auto active = active_;
        const auto generation = generation_;
        if (!nativeRosterIntact())
        {
            endSession(MultiplayerSessionEndReason::Left);
            return;
        }
        std::vector<GameKitMatchPacket> packets;
        packets.swap(active->mailbox->packets);
        for (const auto& packet : packets)
        {
            if (generation != generation_ || active_ != active || !sessionUsable())
                return;
            if (!receiveControl(packet))
            {
                LOG_WARNING << "GameKit invalid session control packet";
                endSession(MultiplayerSessionEndReason::Left);
                return;
            }
        }
        if (generation != generation_ || active_ != active || !sessionUsable())
            return;
        if (isHost())
            beginPlayingIfReady();
        else
            sendGuestAgreement();
    }

    bool begin(MultiplayerOperation operation, const String& version, unsigned int maxMembers)
    {
        if (active_ || pending_ || !matchmaking_.begin(operation, version, 2, maxMembers))
            return false;
        ++generation_;
        pending_ = true;
        operation_ = operation;
        accountGeneration_ = authentication_.getGeneration();
        return true;
    }

    void fail(MultiplayerOperationFailureReason reason)
    {
        const auto operation = operation_;
        ++generation_;
        pending_ = false;
        matchmaking_.cancel();
        negotiation_.cancel();
        dispatchOperationCompleted(operation, reason);
    }

    void adopt(const std::shared_ptr<GameKitActiveMatch>& ready)
    {
        const auto operation = operation_;
        const auto previous = getSession().getState();
        const auto generation = ++generation_;
        active_ = ready;
        pending_ = false;
        mutableSession() = active_->candidate->session;
        // Every public query is coherent before the first callback. A callback
        // may leave or replace this session; do not resume dispatch for it.
        dispatchSessionStateChanged(previous);
        if (generation != generation_ || active_ != ready)
            return;
        dispatchSessionUpdated();
        if (generation == generation_ && active_ == ready)
            dispatchOperationCompleted(operation, true);
    }

    void endSession(MultiplayerSessionEndReason reason)
    {
        const auto generation = ++generation_;
        pending_ = false;
        matchmaking_.cancel();
        negotiation_.cancel();
        if (!active_)
            return;
        auto old = active_;
        active_.reset();
        auto& controller = old->candidate->controller;
        controller.invalidateGeneration();
        controller.discardMetadata();
        controller.clearParticipants();
        const auto ended = controller.finishSession(mutableSession());
        // Disconnect before dispatching callbacks, even if adopt() still holds
        // a shared reference to the old transport during reentrant cancellation.
        old->mailbox.reset();
        old->attempt->match.delegate = nil;
        [old->attempt->match disconnect];
        old->attempt.reset();
        if (ended.previousID && ended.previousState != MultiplayerSessionState::Ended)
            dispatchSessionEnded(ended.previousID, reason);
        if (generation != generation_)
            return;
        dispatchSessionStateChanged(ended.previousState);
        if (generation == generation_ && ended.hadPlayers)
            dispatchSessionUpdated();
    }

    const GameKitAuthentication& authentication_;
    GameKitMatchmaking matchmaking_;
    GameKitNegotiation negotiation_;
    std::shared_ptr<GameKitActiveMatch> active_;
    bool pending_ = false;
    MultiplayerOperation operation_ = MultiplayerOperation::FindLobby;
    uint64_t generation_ = 0;
    uint64_t accountGeneration_ = 0;
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
