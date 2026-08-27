/*
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "CarbonEngine/Common.h"
#include "CarbonEngine/Application.h"
#include "CarbonEngine/Core/CoreEvents.h"
#include "CarbonEngine/Core/EventManager.h"
#include "CarbonEngine/Core/InterfaceRegistry.h"
#include "CarbonEngine/Globals.h"
#include "CarbonEngine/Platform/iOS/PlatformiOS.h"
#include "CarbonEngine/Platform/PlatformEvents.h"

#ifdef iOS

namespace Carbon
{

CARBON_REGISTER_INTERFACE_IMPLEMENTATION(PlatformInterface, PlatformiOS, 100)

// These two methods are defined by the client application when it includes CarbonEngine/EntryPoint.h
extern String iOSGetApplicationName();
extern Application* iOSCreateApplication();

}

// Define the application delegate to use
@interface CarboniOSApplicationDelegate : NSObject <UIApplicationDelegate>
@property (atomic) Carbon::Application* application;
@property (atomic) NSTimer* animationTimer;
- (void)initializeApplication;
- (void)startAnimation;
- (void)stopAnimation;
@end

@implementation CarboniOSApplicationDelegate
@synthesize application;
@synthesize animationTimer;

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
    self.animationTimer = nil;

    // Preserve the legacy application lifecycle on iOS 12 and for clients that have not opted into UIScene.
    if (@available(ios 13.0, *))
    {
        if (![[NSBundle mainBundle] objectForInfoDictionaryKey:@"UIApplicationSceneManifest"])
            [self initializeApplication];
    }
    else
        [self initializeApplication];

    return YES;
}

- (void)initializeApplication
{
    if (self.application)
        return;

    if (!Carbon::Globals::initializeEngine(Carbon::iOSGetApplicationName()))
    {
        LOG_ERROR << "Failed initializing the engine";
        exit(0);
    }

    self.application = Carbon::iOSCreateApplication();
    if (!self.application->run(false))
    {
        LOG_ERROR << "Failed initializing the application";

        delete self.application;
        self.application = nullptr;
    }
}

- (void)startAnimation
{
    if (self.animationTimer)
        [self.animationTimer invalidate];

    self.animationTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 240.0
                                                           target:self
                                                         selector:@selector(tick)
                                                         userInfo:nil
                                                          repeats:YES];
}

- (void)stopAnimation
{
    [self.animationTimer invalidate];
    self.animationTimer = nil;
}

- (void)tick
{
    // Cycle the main loop
    if (self.application && !self.application->mainLoop())
    {
        LOG_ERROR << "iOS applications are not allowed to self-terminate";
        assert(false);
        exit(0);
    }
}

- (void)applicationWillTerminate:(UIApplication*)application
{
    LOG_INFO << "iOS application will terminate";

    [self stopAnimation];

    // Shut down the application
    if (self.application)
        self.application->shutdown();

    // Shut down the engine
    Carbon::Globals::uninitializeEngine();
}

- (void)applicationDidReceiveMemoryWarning:(UIApplication*)application
{
    LOG_INFO << "iOS low memory warning received";
    Carbon::events().dispatchEvent(Carbon::LowMemoryWarningEvent());
}
@end

// Scene delegate used by the modern UIKit scene lifecycle. Carbon supports a single rendering scene.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
@interface CarboniOSSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

@implementation CarboniOSSceneDelegate

- (CarboniOSApplicationDelegate*)applicationDelegate
{
    return static_cast<CarboniOSApplicationDelegate*>(UIApplication.sharedApplication.delegate);
}

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                options:(UISceneConnectionOptions*)connectionOptions
{
    if (![scene isKindOfClass:[UIWindowScene class]])
        return;

    auto windowScene = static_cast<UIWindowScene*>(scene);
    auto window = [[UIWindow alloc] initWithWindowScene:windowScene];
    static_cast<Carbon::PlatformiOS&>(Carbon::platform()).setWindow(window);
    [[self applicationDelegate] initializeApplication];
}

- (void)sceneDidBecomeActive:(UIScene*)scene
{
    LOG_INFO << "iOS scene became active";
    Carbon::events().dispatchEvent(Carbon::ApplicationGainFocusEvent());
    [[self applicationDelegate] startAnimation];
}

- (void)sceneWillResignActive:(UIScene*)scene
{
    LOG_INFO << "iOS scene will resign active";
    Carbon::events().dispatchEvent(Carbon::ApplicationLoseFocusEvent());
    [[self applicationDelegate] stopAnimation];
}

- (void)sceneDidEnterBackground:(UIScene*)scene
{
    LOG_INFO << "iOS scene entered the background";
    Carbon::events().dispatchEvent(Carbon::ApplicationLoseFocusEvent(true));
}

- (void)sceneWillEnterForeground:(UIScene*)scene
{
    LOG_INFO << "iOS scene will enter the foreground";
    Carbon::events().dispatchEvent(Carbon::ApplicationGainFocusEvent(true));
}

@end
#pragma clang diagnostic pop

int main(int argc, char* argv[])
{
    @autoreleasepool
    {
        Carbon::Globals::setInStaticInitialization(false);
        Carbon::Globals::setCommandLineParameters(argc, const_cast<const char**>(argv));

        return UIApplicationMain(argc, argv, nil, @"CarboniOSApplicationDelegate");
    };
}

#endif
