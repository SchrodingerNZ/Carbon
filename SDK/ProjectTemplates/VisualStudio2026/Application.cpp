#include "$projectname$.h"

// This simple Carbon application renders a spinning logo.

bool $safeprojectname$::initialize()
{
    // Create a scene with a 2D camera
    scene_.setName("Test");
    scene_.create2DCamera(0.0f, 100.0f);

    // Put a sprite with the logo on it in the center of the screen
    sprite_ = scene_.addEntity<Sprite>("SpriteName", 50.0f, 50.0f);
    sprite_->setSpriteTexture("CarbonLogo.png");
    sprite_->alignToScreen(Sprite::ScreenMiddle);

    return true;
}

void $safeprojectname$::frameUpdate()
{
    // Rotate the logo sprite
    sprite_->rotateAroundCenter(platform().getSecondsPassed());
}

void $safeprojectname$::queueScenes()
{
    scene_.queueForRendering();
}

bool $safeprojectname$::onKeyDownEvent(const KeyDownEvent& e)
{
    return CARBON_APPLICATION_CLASS::onKeyDownEvent(e);
}

#define CARBON_ENTRY_POINT_CLASS $safeprojectname$
#include "CarbonEngine/EntryPoint.h"