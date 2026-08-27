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

@class CarboniOSSceneDelegate;

// Define the application delegate to use
@interface CarboniOSApplicationDelegate : NSObject <UIApplicationDelegate>
@property (atomic) Carbon::Application* application;
@property (atomic) NSTimer* animationTimer;
- (void)initializeApplicationForWindowScene:(UIWindowScene*)windowScene;
- (void)startAnimation;
- (void)stopAnimation;
@end

@implementation CarboniOSApplicationDelegate
@synthesize application;
@synthesize animationTimer;

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
    self.animationTimer = nil;

    // Preserve the legacy application lifecycle for clients that have not yet opted into UIScene in their Info.plist.
    if (![[NSBundle mainBundle] objectForInfoDictionaryKey:@"UIApplicationSceneManifest"])
        [self initializeApplicationForWindowScene:nil];

    return YES;
}

- (void)initializeApplicationForWindowScene:(UIWindowScene*)windowScene
{
    if (self.application)
        return;

    if (!Carbon::Globals::initializeEngine(Carbon::iOSGetApplicationName()))
    {
        LOG_ERROR << "Failed initializing the engine";
        exit(0);
    }

    static_cast<Carbon::PlatformiOS&>(Carbon::platform()).setWindowScene(windowScene);

    self.application = Carbon::iOSCreateApplication();
    if (!self.application->run(false))
    {
        LOG_ERROR << "Failed initializing the application";

        delete self.application;
        self.application = nullptr;
    }
}

- (UISceneConfiguration*)application:(UIApplication*)application
    configurationForConnectingSceneSession:(UISceneSession*)connectingSceneSession
                                    options:(UISceneConnectionOptions*)options
{
    auto configuration = [[UISceneConfiguration alloc] initWithName:@"Carbon Default Configuration"
                                                         sessionRole:connectingSceneSession.role];
    configuration.delegateClass = [CarboniOSSceneDelegate class];
    return configuration;
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
@interface CarboniOSSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

@implementation CarboniOSSceneDelegate

- (CarboniOSApplicationDelegate*)applicationDelegate
{
    return (CarboniOSApplicationDelegate*)UIApplication.sharedApplication.delegate;
}

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                options:(UISceneConnectionOptions*)connectionOptions
{
    if (![scene isKindOfClass:[UIWindowScene class]])
        return;

    [[self applicationDelegate] initializeApplicationForWindowScene:(UIWindowScene*)scene];
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
