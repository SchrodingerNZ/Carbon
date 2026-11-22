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
#include "CarbonEngine/Core/Memory/MemoryInterceptor.h"

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

class GameKitMultiplayer : public MultiplayerInterface
{
public:
    explicit GameKitMultiplayer(const GameKitAuthentication& authentication) : authentication_(authentication) {}
    String getLocalPlayerName() const override { return authentication_.getPlayerName(); }

private:
    const GameKitAuthentication& authentication_;
};

class GameKitConnectionPlatform : public ConnectionPlatformInterface
{
public:
    GameKitConnectionPlatform() : multiplayer_(authentication_) {}

    void update() override { authentication_.update(); }

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
