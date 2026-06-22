#include "vrinputmanager.hpp"

#include "openxrinput.hpp"
#include "vranimation.hpp"
#include "vrgui.hpp"
#include "vrpointer.hpp"

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/sdlutil/sdlmappings.hpp>
#include <components/vr/session.hpp>
#include <components/vr/trackingmanager.hpp>
#include <components/vr/viewer.hpp>
#include <components/vr/vr.hpp>
#include <components/xr/action.hpp>
#include <components/xr/actionset.hpp>
#include <components/xr/instance.hpp>
#include <components/xr/session.hpp>

#include <MyGUI_InputManager.h>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwgui/draganddrop.hpp"
#include "../mwgui/inventorywindow.hpp"

#include "../mwinput/actionmanager.hpp"
#include "../mwinput/bindingsmanager.hpp"
#include "../mwinput/controllermanager.hpp"
#include "../mwinput/mousemanager.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/movement.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/player.hpp"

#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"

#include <oics/ICSInputControlSystem.h>

#include <SDL_timer.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <variant>

namespace MWVR
{
    namespace
    {
        float getXrFloat(XR::ActionSet& actionSet, const std::string& path)
        {
            const auto value = actionSet.getValue(path);
            if (!value)
                return 0.f;
            if (const auto* number = std::get_if<float>(&*value))
                return *number;
            return 0.f;
        }

        float vrMovementDeadzone(float value)
        {
            const float magnitude = std::abs(value);
            if (magnitude < 0.2f)
                return 0.f;
            return std::copysign(std::min(1.f, (magnitude - 0.2f) / 0.6f), value);
        }

        osg::Vec2f sFallbackMovementInput;
        bool sHasFallbackMovementInput = false;
        float sFallbackSnapTurnYaw = 0.f;
        bool sSnapTurnLatched = false;
    }

    bool hasFallbackMovementInput()
    {
        return sHasFallbackMovementInput;
    }

    osg::Vec2f getFallbackMovementInput()
    {
        return sFallbackMovementInput;
    }

    bool consumeFallbackSnapTurn(float& yawRadians)
    {
        if (sFallbackSnapTurnYaw == 0.f)
            return false;

        yawRadians = sFallbackSnapTurnYaw;
        sFallbackSnapTurnYaw = 0.f;
        return true;
    }

    void VRInputManager::updateVRPointer(bool disableControls)
    {
        std::shared_ptr<VR::Space> source;
        std::string sourceName;
        if (VR::getVR())
        {
            const bool guiMode = MWBase::Environment::get().getWindowManager()->isGuiMode();
            const bool leftPointer = mPointerLeft || guiMode;
            const bool rightPointer = mPointerRight || guiMode;

            if (VR::getLeftHandedMode() && leftPointer)
            {
                source = mXRInput->getSpace(VR::Paths::LEFT_HAND_AIM);
                sourceName = "LeftHandAim";
            }
            else if (rightPointer)
            {
                source = mXRInput->getSpace(VR::Paths::RIGHT_HAND_AIM);
                sourceName = "RightHandAim";
            }
            else if (leftPointer)
            {
                source = mXRInput->getSpace(VR::Paths::LEFT_HAND_AIM);
                sourceName = "LeftHandAim";
            }
        }
        if (!source)
        {
            sourceName = "None";
        }

        if (sourceName != mLastPointerSourceName || mPointerSourceLogFrames < 20
            || (mPointerSourceLogFrames % 300) == 0)
        {
            Log(Debug::Info) << "FNV/ESM4 diag: VR pointer source=" << sourceName
                             << " vr=" << VR::getVR()
                             << " leftPointer=" << mPointerLeft
                             << " rightPointer=" << mPointerRight
                             << " disableControls=" << disableControls
                             << " guiMode=" << MWBase::Environment::get().getWindowManager()->isGuiMode();
            mLastPointerSourceName = sourceName;
        }
        ++mPointerSourceLogFrames;

        if (!source)
        {
            if (mVRPointer)
                mVRPointer->hide();
            return;
        }

        if (!mVRPointer && !disableControls)
        {
            osg::ref_ptr<osgViewer::Viewer> viewer;
            mOSGViewer.lock(viewer);
            if (viewer)
                mVRPointer = std::make_unique<UserPointer>(viewer->getSceneData()->asGroup());
        }

        if (mVRPointer)
        {
            mVRPointer->setSource(source);
            mVRPointer->setDebugSpaces(
                mXRInput->getSpace(VR::Paths::LEFT_HAND_AIM), mXRInput->getSpace(VR::Paths::RIGHT_HAND_AIM));
            mVRPointer->update();
        }
    }

    void VRInputManager::updateRealisticCombat(float dt)
    {
        auto ptr = MWBase::Environment::get().getWorld()->getPlayerPtr();
        auto* anim = MWBase::Environment::get().getWorld()->getAnimation(ptr);
        auto* vrAnim = static_cast<MWVR::VRAnimation*>(anim);
        mVRAimNode = vrAnim->getWeaponTransform();
    }

    void VRInputManager::updateFallbackMovement(bool disableControls)
    {
        if (!VR::getVR())
            return;

        auto state = MWBase::Environment::get().getStateManager();
        auto window = MWBase::Environment::get().getWindowManager();
        auto world = MWBase::Environment::get().getWorld();
        auto lua = MWBase::Environment::get().getLuaManager();
        if (state->getState() == MWBase::StateManager::State_NoGame)
            return;

        const bool blocked = disableControls || window->isGuiMode();
        const std::string hand = VR::getLeftHandedMode() ? "/user/hand/right" : "/user/hand/left";
        const std::string turnHand = VR::getLeftHandedMode() ? "/user/hand/left" : "/user/hand/right";
        auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
        const float side = blocked ? 0.f : vrMovementDeadzone(getXrFloat(actionSet, hand + "/input/thumbstick/x"));
        const float movement = blocked ? 0.f : vrMovementDeadzone(getXrFloat(actionSet, hand + "/input/thumbstick/y"));
        const float turn = blocked ? 0.f : getXrFloat(actionSet, turnHand + "/input/thumbstick/x");
        const bool active = std::abs(side) > 0.f || std::abs(movement) > 0.f;
        sFallbackMovementInput = osg::Vec2f(side, movement);
        sHasFallbackMovementInput = active;
        if (std::abs(turn) < 0.35f)
            sSnapTurnLatched = false;
        else if (!sSnapTurnLatched && std::abs(turn) > 0.75f)
        {
            sFallbackSnapTurnYaw = (turn > 0.f ? 1.f : -1.f) * osg::DegreesToRadians(30.f);
            sSnapTurnLatched = true;
            Log(Debug::Info) << "FNV/ESM4 diag: VR fallback snap turn"
                             << " hand=" << turnHand << " x=" << turn
                             << " yawRadians=" << sFallbackSnapTurnYaw;
        }

        if (!active && !mFallbackMovementActive)
            return;

        MWWorld::Ptr player = world->getPlayerPtr();
        if (player.isEmpty())
            return;

        if (MWBase::LuaManager::ActorControls* controls = lua->getActorControls(player))
        {
            controls->mSideMovement = side;
            controls->mMovement = movement;
            controls->mChanged = true;
        }

        auto& movementSettings = player.getClass().getMovementSettings(player);
        movementSettings.mPosition[0] = side;
        movementSettings.mPosition[1] = movement;
        movementSettings.mSpeedFactor = osg::Vec2f(side, movement).length();

        static bool sHaveLastPlayerPos = false;
        static osg::Vec3f sLastPlayerPos;
        const osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
        const osg::Vec3f playerDelta = sHaveLastPlayerPos ? playerPos - sLastPlayerPos : osg::Vec3f();
        sLastPlayerPos = playerPos;
        sHaveLastPlayerPos = true;

        if ((active || mFallbackMovementActive) && mFallbackMovementLogCount < 32)
        {
            ++mFallbackMovementLogCount;
            Log(Debug::Info) << "FNV/ESM4 diag: VR fallback movement"
                             << " hand=" << hand << " side=" << side << " forward=" << movement
                             << " blocked=" << blocked << " active=" << active
                             << " playerPos=(" << playerPos.x() << "," << playerPos.y() << "," << playerPos.z()
                             << ") playerDelta=(" << playerDelta.x() << "," << playerDelta.y() << ","
                             << playerDelta.z() << ") settings=(" << movementSettings.mPosition[0] << ","
                             << movementSettings.mPosition[1] << "," << movementSettings.mPosition[2]
                             << ") speedFactor=" << movementSettings.mSpeedFactor;
        }

        mFallbackMovementActive = active;
    }

    void VRInputManager::updateVoidMessages()
    {
        auto wm = MWBase::Environment::get().getWindowManager();
        if (wm->isInteractiveMessageBoxActive())
            return;
        if (mVoidMessages.size() > 0)
        {
            wm->interactiveMessageBox(mVoidMessages.front(), { "#{Interface:OK}" });
            mVoidMessages.pop();
        }
    }

    MWWorld::Ptr VRInputManager::getPointerTarget() const
    {
        if (mVRPointer)
        {
            const auto& ray = mVRPointer->getPointerRay();
            if (ray.mHit)
                return ray.mHitObject;
        }
        return MWWorld::Ptr();
    }

    void VRInputManager::setScrollSpeed(float speed)
    {
        mScrollSpeed = speed;
    }

    void VRInputManager::gameMenuAction(bool onPress) {
        if (onPress)
            mGameMenuLongPressTimer = std::chrono::steady_clock::now();
        else if (mGameMenuLongPressTimer)
        {
            mGameMenuLongPressTimer = std::nullopt;
            executeAction(MWInput::A_GameMenu);
        }
    }

    void VRInputManager::showMessageInTheVoid(std::string_view message) {
        mVoidMessages.push(std::string(message));
    }

    void VRInputManager::injectChannelValue(MWInput::Actions action, float value)
    {
        Log(Debug::Info) << "FNV/ESM4 diag: VR channel injection pending public binding route"
                         << " action=" << static_cast<int>(action) << " value=" << value;
    }

    int VRInputManager::interactiveMessageBox(const std::string& message, const std::vector<std::string>& buttons)
    {
        auto wm = MWBase::Environment::get().getWindowManager();

        wm->interactiveMessageBox(message, buttons, true);

        return wm->readPressedButton();
    }

    // void VRInputManager::updatePhysicalSneak(Stereo::Unit headsetHeight)
    //{
    //     // Do physical sneak toggle if necessary
    //     const auto playerHeight = VR::Session::instance().playerHeight();
    //     if (mPhysicalSneakEnabled && VR::getStandingPlay() && playerHeight.asMeters() > 0.0f)
    //     {
    //         mIsPhysicalSneak = headsetHeight < playerHeight - mPhysicalSneakHeightOffset;
    //     }
    // }

    void VRInputManager::setPointerLeft(bool enabled)
    {
        mPointerLeft = enabled;
    }

    bool VRInputManager::getPointerLeft() const
    {
        return mPointerLeft;
    }

    void VRInputManager::setPointerRight(bool enabled)
    {
        mPointerRight = enabled;
    }

    bool VRInputManager::getPointerRight() const
    {
        return mPointerRight;
    }

    void VRInputManager::pointerActivate(bool injectMouseClickIfApplicable)
    {
        if (injectMouseClickIfApplicable
            && (MWVR::VRGUIManager::instance().hasFocus()
                || MWVR::VRGUIManager::instance().hasAndroidOpenXRQuadFocusLatch()))
        {
            auto cursor = MWVR::VRGUIManager::instance().guiCursor();
            if (MWVR::VRGUIManager::instance().injectMouseClick())
                return;

#if defined(__ANDROID__)
            Log(Debug::Info) << "FNV/ESM4 proof: Android VR pointer GUI click fallback unavailable cursor="
                             << cursor.x() << "," << cursor.y();
#endif
            return;
        }

        if (mVRPointer)
            mVRPointer->activate();
    }

    void VRInputManager::pointerActivateDelayed(bool injectMouseClickIfApplicable)
    {
        mDelayedPointerActivate = true;
        mDelayedPointerActivateInjectMouseClickIfApplicable = injectMouseClickIfApplicable;
    }

    static VRInputManager* sInputManager;

    VRInputManager::VRInputManager(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer,
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler, const std::filesystem::path& userFile,
        bool userFileExists, const std::filesystem::path& userControllerBindingsFile,
        const std::filesystem::path& controllerBindingsFile, bool grab)
        : MWInput::InputManager(window, viewer, screenCaptureHandler, userFile, userFileExists,
              userControllerBindingsFile, controllerBindingsFile, grab)
        , mOSGViewer(viewer)
        , mXRInput(new OpenXRInput())
    {

        sInputManager = this;
    }

    VRInputManager::~VRInputManager() {}

    VRInputManager& VRInputManager::instance()
    {
        assert(sInputManager);
        return *sInputManager;
    }

    void VRInputManager::changeInputMode(bool mode)
    {
        // VR mode has no concept of these
        // mGuiCursorEnabled = false;
        MWInput::InputManager::changeInputMode(mode);
        MWBase::Environment::get().getWindowManager()->showCrosshair(false);
    }

    void VRInputManager::update(float dt, bool disableControls, bool disableEvents)
    {
        mDt = dt;
        if (mDelayedPointerActivate)
        {
            mDelayedPointerActivate = false;
            pointerActivate(mDelayedPointerActivateInjectMouseClickIfApplicable);
        }

        updateVRPointer(disableControls);

        if (MWVR::VRGUIManager::instance().hasFocus())
        {
            auto guiCursor = MWVR::VRGUIManager::instance().guiCursor();
            (void)guiCursor;
            if (std::abs(mScrollSpeed) > 0.01f)
            {
                mScrollPoints += mScrollSpeed * 750.f * dt;

                if (std::abs(mScrollPoints) >= 1.f)
                {

                    SDL_MouseWheelEvent arg;
                    arg.type = SDL_MOUSEWHEEL;
                    arg.x = 0;
                    arg.y = mScrollSpeed > 0 ? 1 : -1;
                    arg.direction = SDL_MOUSEWHEEL_NORMAL;

                    mScrollPoints -= std::floor(mScrollPoints);
                }
            }
        }

        auto wm = MWBase::Environment::get().getWindowManager();

        if (VR::getVR())
        {
            auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
            actionSet.update();
        }

        if (mGameMenuLongPressTimer)
        {
            auto elapsed = std::chrono::steady_clock::now() - mGameMenuLongPressTimer.value();
            auto longPressTime = std::chrono::duration<double>(2.0 / 3.0);
            if (elapsed > longPressTime)
            {
                VR::recenterXY();
                mGameMenuLongPressTimer = std::nullopt;
            }
        }

        MWInput::InputManager::update(dt, disableControls, disableEvents);
        updateFallbackMovement(disableControls || disableEvents);

        updateVoidMessages();

        // The rest of this code assumes the game is running
        if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame)
            return;

        if (VR::getVR())
            updateVrDebugSnapshotControls();

        bool guiMode = wm->isGuiMode();

        // OpenMW assumes all input will come via SDL which i often violate.
        // This keeps player controls correctly enabled for my purposes.
        toggleControlSwitch("playercontrols", !guiMode);
    }

    void VRInputManager::onSpaceUpdate()
    {
        if (VR::getVR()
            && MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_Running)
            updateRealisticCombat(mDt);
    }

}
