#include "vrinputmanager.hpp"

#include "fnvxrliveframesurface.hpp"
#include "openxrinput.hpp"
#include "realisticcombat.hpp"
#include "vranimation.hpp"
#include "vrgui.hpp"
#include "vrpointer.hpp"
#include "vrutil.hpp"

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
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwgui/draganddrop.hpp"
#include "../mwgui/inventorywindow.hpp"
#include "../mwgui/mode.hpp"

#include "../mwinput/actionmanager.hpp"
#include "../mwinput/bindingsmanager.hpp"
#include "../mwinput/controllermanager.hpp"
#include "../mwinput/mousemanager.hpp"

#include "../mwmechanics/movement.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/player.hpp"

#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"

#include <extern/oics/ICSInputControlSystem.h>

#include <SDL_timer.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <variant>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef DrawState
#undef DrawState
#endif
#endif

namespace MWVR
{
    namespace
    {
#ifdef _WIN32
        constexpr std::uint32_t XInputSharedMagic = 0x58564e46; // FNVX
        constexpr std::uint32_t XInputSharedVersion = 1;
        constexpr std::uint16_t XInputDpadUp = 0x0001;
        constexpr std::uint16_t XInputDpadDown = 0x0002;
        constexpr std::uint16_t XInputDpadLeft = 0x0004;
        constexpr std::uint16_t XInputDpadRight = 0x0008;
        constexpr std::uint16_t XInputStart = 0x0010;
        constexpr std::uint16_t XInputBack = 0x0020;
        constexpr std::uint16_t XInputA = 0x1000;
        constexpr std::uint16_t XInputB = 0x2000;
        constexpr std::uint16_t XInputX = 0x4000;
        constexpr std::uint16_t XInputY = 0x8000;

        struct SharedXInputState
        {
            std::uint32_t magic;
            std::uint32_t version;
            std::uint32_t packet;
            std::uint16_t buttons;
            std::uint8_t leftTrigger;
            std::uint8_t rightTrigger;
            std::int16_t leftThumbX;
            std::int16_t leftThumbY;
            std::int16_t rightThumbX;
            std::int16_t rightThumbY;
            std::uint8_t connected;
            std::uint8_t reserved[3];
        };
        static_assert(sizeof(SharedXInputState) == 28, "SharedXInputState layout changed");

        HANDLE sXInputMapping = nullptr;
        SharedXInputState* sXInputState = nullptr;
        bool sLoggedXInputMapping = false;
#endif

        bool actionPressed(const XR::ActionSet& actionSet, const std::initializer_list<const char*>& ids)
        {
            for (const char* id : ids)
            {
                std::optional<XR::InputAction::Value> value;
                try
                {
                    value = actionSet.getValue(id);
                }
                catch (const std::out_of_range&)
                {
                    continue;
                }
                if (!value)
                    continue;

                if (const bool* pressed = std::get_if<bool>(&*value))
                {
                    if (*pressed)
                        return true;
                }
                else if (const float* amount = std::get_if<float>(&*value))
                {
                    if (*amount > 0.55f)
                        return true;
                }
            }
            return false;
        }

        float actionFloat(const XR::ActionSet& actionSet, const std::initializer_list<const char*>& ids)
        {
            for (const char* id : ids)
            {
                std::optional<XR::InputAction::Value> value;
                try
                {
                    value = actionSet.getValue(id);
                }
                catch (const std::out_of_range&)
                {
                    continue;
                }
                if (!value)
                    continue;

                if (const float* amount = std::get_if<float>(&*value))
                    return *amount;
                if (const bool* pressed = std::get_if<bool>(&*value); pressed && *pressed)
                    return 1.f;
            }
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

        std::int16_t thumbAxis(float value)
        {
            value = std::clamp(value, -1.f, 1.f);
            return static_cast<std::int16_t>(std::lround(value * 32767.f));
        }

#ifdef _WIN32
        SharedXInputState* sharedXInputState()
        {
            if (sXInputState)
                return sXInputState;

            sXInputMapping = OpenFileMappingA(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, "Local\\FNVXR_XInput_State");
            if (!sXInputMapping)
                return nullptr;

            sXInputState = static_cast<SharedXInputState*>(
                MapViewOfFile(sXInputMapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(SharedXInputState)));
            if (!sXInputState)
            {
                CloseHandle(sXInputMapping);
                sXInputMapping = nullptr;
                return nullptr;
            }

            if (!sLoggedXInputMapping)
            {
                sLoggedXInputMapping = true;
                Log(Debug::Info) << "FNVXR retail surface: mapped Local\\FNVXR_XInput_State";
            }
            return sXInputState;
        }
#endif

        void rootPlayerMovementForRetailSurface()
        {
            if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame)
                return;

            try
            {
                auto playerPtr = MWBase::Environment::get().getWorld()->getPlayerPtr();
                auto& movement = playerPtr.getClass().getMovementSettings(playerPtr);
                movement.mPosition[0] = 0.f;
                movement.mPosition[1] = 0.f;
                movement.mPosition[2] = 0.f;
                movement.mRotation[0] = 0.f;
                movement.mRotation[1] = 0.f;
                movement.mRotation[2] = 0.f;
                movement.mSpeedFactor = 0.f;
                movement.mIsStrafing = false;
            }
            catch (const std::exception& e)
            {
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    Log(Debug::Warning) << "FNVXR retail surface: failed to root OpenMW movement: " << e.what();
                }
            }
        }

        void suppressOpenMWGuiForRetailSurface()
        {
            auto windowManager = MWBase::Environment::get().getWindowManager();
            const MWGui::GuiMode blockedModes[] = {
                MWGui::GM_Inventory,
                MWGui::GM_Journal,
                MWGui::GM_QuickKeysMenu,
                MWGui::GM_RadialMenu,
                MWGui::GM_VrMetaMenu,
            };

            for (MWGui::GuiMode mode : blockedModes)
            {
                if (windowManager->containsMode(mode))
                    windowManager->removeGuiMode(mode);
            }
        }

        enum class RetailSurfaceGate
        {
            Ready,
            ControlsDisabled,
            NotRunning,
            GuiMode,
            NoPlayer,
            NoAnimation,
            Exception,
        };

        const char* retailSurfaceGateName(RetailSurfaceGate gate)
        {
            switch (gate)
            {
                case RetailSurfaceGate::Ready:
                    return "ready";
                case RetailSurfaceGate::ControlsDisabled:
                    return "controls-disabled";
                case RetailSurfaceGate::NotRunning:
                    return "not-running";
                case RetailSurfaceGate::GuiMode:
                    return "gui-mode";
                case RetailSurfaceGate::NoPlayer:
                    return "no-player";
                case RetailSurfaceGate::NoAnimation:
                    return "no-animation";
                case RetailSurfaceGate::Exception:
                    return "exception";
            }

            return "unknown";
        }

        RetailSurfaceGate retailSurfaceBridgeGate(bool disableControls)
        {
            if (disableControls)
                return RetailSurfaceGate::ControlsDisabled;

            try
            {
                auto stateManager = MWBase::Environment::get().getStateManager();
                if (stateManager->getState() != MWBase::StateManager::State_Running)
                    return RetailSurfaceGate::NotRunning;

                auto windowManager = MWBase::Environment::get().getWindowManager();
                if (windowManager->isGuiMode())
                    return RetailSurfaceGate::GuiMode;

                auto world = MWBase::Environment::get().getWorld();
                MWWorld::Ptr playerPtr = world->getPlayerPtr();
                if (playerPtr.isEmpty())
                    return RetailSurfaceGate::NoPlayer;

                if (!world->getAnimation(playerPtr))
                    return RetailSurfaceGate::NoAnimation;

                return RetailSurfaceGate::Ready;
            }
            catch (const std::exception&)
            {
                return RetailSurfaceGate::Exception;
            }
        }

        bool retailSurfaceBridgeReady(bool disableControls)
        {
            return retailSurfaceBridgeGate(disableControls) == RetailSurfaceGate::Ready;
        }
    }

    bool hasFallbackMovementInput()
    {
        return sHasFallbackMovementInput;
    }

    osg::Vec2f getFallbackMovementInput()
    {
        return sFallbackMovementInput;
    }

    void VRInputManager::updateVRPointer(bool disableControls)
    {
        std::shared_ptr<VR::Space> source;
        std::string sourceName;
        if (VR::getVR())
        {
            bool guiMode = MWBase::Environment::get().getWindowManager()->isGuiMode();
            const bool retailSurfaceModal = MWVR::FNVXRLiveFrameSurface::instance().modalInputActive();
            auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
            const bool directLeftPointer = actionPressed(actionSet, {
                "/user/hand/left/input/squeeze/value",
                "/user/hand/left/input/squeeze/click",
            });
            const bool directRightPointer = actionPressed(actionSet, {
                "/user/hand/right/input/squeeze/value",
                "/user/hand/right/input/squeeze/click",
            });
            bool leftPointer = mPointerLeft || directLeftPointer || guiMode || retailSurfaceModal;
            bool rightPointer = mPointerRight || directRightPointer || guiMode || retailSurfaceModal;

            static bool sLastDirectLeftPointer = false;
            static bool sLastDirectRightPointer = false;
            if (directLeftPointer != sLastDirectLeftPointer || directRightPointer != sLastDirectRightPointer)
            {
                Log(Debug::Info) << "OpenMW VR pointer input leftSqueeze=" << directLeftPointer
                                 << " rightSqueeze=" << directRightPointer
                                 << " luaLeft=" << mPointerLeft << " luaRight=" << mPointerRight;
                sLastDirectLeftPointer = directLeftPointer;
                sLastDirectRightPointer = directRightPointer;
            }

            if (!disableControls && !retailSurfaceModal)
                MWBase::Environment::get().getWorld()->enableVRPointer(leftPointer, rightPointer);
            else if (retailSurfaceModal)
                MWBase::Environment::get().getWorld()->enableVRPointer(false, false);

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
            source = mXRInput->getSpace(OpenXRInput::DefaultReferenceSpaceView);
            sourceName = "DefaultReferenceSpaceView";
        }

        if (sourceName != mLastPointerSourceName || mPointerSourceLogFrames < 20 || (mPointerSourceLogFrames % 300) == 0)
        {
            Log(Debug::Verbose) << "FNV/ESM4 diag: VR pointer source=" << sourceName
                             << " vr=" << VR::getVR()
                             << " leftPointer=" << mPointerLeft
                             << " rightPointer=" << mPointerRight
                             << " disableControls=" << disableControls
                             << " guiMode=" << MWBase::Environment::get().getWindowManager()->isGuiMode()
                             << " retailModal=" << MWVR::FNVXRLiveFrameSurface::instance().modalInputActive();
            mLastPointerSourceName = sourceName;
        }
        ++mPointerSourceLogFrames;

        // Menus disable world controls, but their 3D panels still require the VR
        // pointer. Construct it as soon as the VR viewer exists instead of waiting
        // for the first gameplay frame.
        if (!mVRPointer && VR::getVR())
        {
            osg::ref_ptr<osgViewer::Viewer> viewer;
            mOSGViewer.lock(viewer);
            if (viewer)
            {
                mVRPointer = std::make_unique<UserPointer>(viewer->getSceneData()->asGroup());
            }
        }

        if (mVRPointer)
        {
            const bool pointerEnabled = sourceName != "DefaultReferenceSpaceView";
            mPointerActive = pointerEnabled;
            mVRPointer->setEnabled(pointerEnabled);
            mVRPointer->setSource(source);
            mVRPointer->setDebugSpaces(
                mXRInput->getSpace(OpenXRInput::LeftHandAim), mXRInput->getSpace(OpenXRInput::RightHandAim));
            mVRPointer->update();
        }
        else
            mPointerActive = false;
    }

    void VRInputManager::updateDirectPointerClick()
    {
        auto& retailSurface = MWVR::FNVXRLiveFrameSurface::instance();
        if (!mPointerActive || retailSurface.visible())
        {
            mDirectPointerClickDown = false;
            return;
        }

        auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
        const bool pressed = actionPressed(actionSet, {
            "/user/hand/right/input/trigger/value",
            "/user/hand/right/input/trigger/click",
            "/user/hand/left/input/trigger/value",
            "/user/hand/left/input/trigger/click",
        });

        if (pressed && !mDirectPointerClickDown)
        {
            if (mDirectPointerClickLogCount < 24)
            {
                ++mDirectPointerClickLogCount;
                Log(Debug::Info) << "OpenMW VR pointer trigger edge guiFocus="
                                 << MWVR::VRGUIManager::instance().hasFocus();
            }
            pointerActivate(true);
        }

        mDirectPointerClickDown = pressed;
    }

    void VRInputManager::updateRetailSurfaceDirectClick(bool retailSurfaceReady)
    {
        auto& retailSurface = MWVR::FNVXRLiveFrameSurface::instance();
        if (!retailSurfaceReady || !retailSurface.visible())
        {
            mRetailSurfaceClickDown = false;
            return;
        }

        auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
        const bool pressed = actionPressed(actionSet, {
            "/user/hand/right/input/trigger/value",
            "/user/hand/right/input/trigger/click",
            "/user/hand/left/input/trigger/value",
            "/user/hand/left/input/trigger/click",
        });

        if (pressed && !mRetailSurfaceClickDown)
        {
            if (mRetailSurfaceDirectClickLogCount < 24)
            {
                ++mRetailSurfaceDirectClickLogCount;
                Log(Debug::Info) << "FNVXR retail surface: direct OpenXR click edge focus="
                                 << retailSurface.hasFocus();
            }
            retailSurface.injectMouseClick();
        }

        mRetailSurfaceClickDown = pressed;
    }

    void VRInputManager::updateRetailSurfaceGripRecenter(bool disableControls)
    {
        auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
        const bool leftGripPressed = actionPressed(actionSet, {
            "/user/hand/left/input/squeeze/value",
            "/user/hand/left/input/squeeze/click",
            "/user/hand/left/input/grip/value",
            "/user/hand/left/input/grip/click",
        });
        const bool rightGripPressed = actionPressed(actionSet, {
            "/user/hand/right/input/squeeze/value",
            "/user/hand/right/input/squeeze/click",
            "/user/hand/right/input/grip/value",
            "/user/hand/right/input/grip/click",
        });
        const bool rightStickClicked = actionPressed(actionSet, {
            "/user/hand/right/input/thumbstick/click",
        });
        const bool rightRecenterChord = rightGripPressed && rightStickClicked;

        auto& retailSurface = MWVR::FNVXRLiveFrameSurface::instance();
        const RetailSurfaceGate gate = retailSurfaceBridgeGate(disableControls);
        if (gate != RetailSurfaceGate::Ready)
        {
            if ((leftGripPressed || rightRecenterChord) && mRetailSurfaceStartupGateLogCount < 24)
            {
                ++mRetailSurfaceStartupGateLogCount;
                Log(Debug::Info) << "FNVXR retail surface: ignored startup grip gate="
                                 << retailSurfaceGateName(gate)
                                 << " leftGrip=" << leftGripPressed
                                 << " rightRecenter=" << rightRecenterChord;
            }
            mRetailSurfaceLeftGripDown = leftGripPressed;
            mRetailSurfaceRightGripDown = rightRecenterChord;
            return;
        }

        if (leftGripPressed && !mRetailSurfaceLeftGripDown)
        {
            Log(Debug::Info) << "FNVXR retail surface: left grip requested retail sidecar";
            retailSurface.recenterMenuPortal();
        }
        if (rightRecenterChord && !mRetailSurfaceRightGripDown)
        {
            Log(Debug::Info) << "FNVXR retail surface: right grip plus right stick recentered retail sidecar";
            retailSurface.recenterMenuPortal();
        }

        mRetailSurfaceLeftGripDown = leftGripPressed;
        mRetailSurfaceRightGripDown = rightRecenterChord;
    }

    void VRInputManager::updateRetailSurfaceXInput(bool retailSurfaceReady)
    {
#ifdef _WIN32
        auto& retailSurface = MWVR::FNVXRLiveFrameSurface::instance();
        auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
        const bool gripHeld = actionPressed(actionSet, {
            "/user/hand/left/input/squeeze/value",
            "/user/hand/left/input/squeeze/click",
            "/user/hand/left/input/grip/value",
            "/user/hand/left/input/grip/click",
            "/user/hand/right/input/squeeze/value",
            "/user/hand/right/input/squeeze/click",
            "/user/hand/right/input/grip/value",
            "/user/hand/right/input/grip/click",
        });
        const bool xboxMode = retailSurface.modalInputActive();

        const bool retailSurfaceModal = retailSurfaceReady && retailSurface.modalInputActive();
        if (!retailSurfaceModal)
        {
            if (auto* shared = sharedXInputState())
            {
                shared->connected = 0;
                ++shared->packet;
            }
            return;
        }

        if (xboxMode || gripHeld)
            rootPlayerMovementForRetailSurface();

        auto* shared = sharedXInputState();
        if (!shared)
            return;

        const float leftX = actionFloat(actionSet, {
            "/user/hand/left/input/thumbstick/x",
            "/user/hand/left/input/trackpad/x",
        });
        const float leftY = actionFloat(actionSet, {
            "/user/hand/left/input/thumbstick/y",
            "/user/hand/left/input/trackpad/y",
        });
        const float rightX = actionFloat(actionSet, {
            "/user/hand/right/input/thumbstick/x",
            "/user/hand/right/input/trackpad/x",
        });
        const float rightY = actionFloat(actionSet, {
            "/user/hand/right/input/thumbstick/y",
            "/user/hand/right/input/trackpad/y",
        });
        constexpr float dpadDeadzone = 0.45f;
        const float navX = std::abs(rightX) > std::abs(leftX) ? rightX : leftX;
        const float navY = std::abs(rightY) > std::abs(leftY) ? rightY : leftY;
        std::uint16_t buttons = 0;
        if (xboxMode && navY > dpadDeadzone)
            buttons |= XInputDpadUp;
        if (xboxMode && navY < -dpadDeadzone)
            buttons |= XInputDpadDown;
        if (xboxMode && navX < -dpadDeadzone)
            buttons |= XInputDpadLeft;
        if (xboxMode && navX > dpadDeadzone)
            buttons |= XInputDpadRight;
        if (xboxMode)
        {
            if (actionPressed(actionSet, { "/user/hand/right/input/a/click", "/user/hand/left/input/x/click",
                                          "/user/hand/right/input/select/click", "/user/hand/left/input/select/click" }))
                buttons |= XInputA;
            if (actionPressed(actionSet, { "/user/hand/right/input/b/click", "/user/hand/left/input/y/click",
                                          "/user/hand/right/input/menu/click", "/user/hand/left/input/menu/click" }))
                buttons |= XInputB;
            if (actionPressed(actionSet, { "/user/hand/right/input/x/click", "/user/hand/left/input/a/click" }))
                buttons |= XInputX;
            if (actionPressed(actionSet, { "/user/hand/right/input/y/click", "/user/hand/left/input/b/click" }))
                buttons |= XInputY;
            if (actionPressed(actionSet, { "/user/hand/left/input/thumbstick/click" }))
                buttons |= XInputBack;
            if (actionPressed(actionSet, { "/user/hand/right/input/thumbstick/click" }))
                buttons |= XInputStart;
        }

        shared->magic = XInputSharedMagic;
        shared->version = XInputSharedVersion;
        shared->connected = xboxMode ? 1 : 0;
        shared->buttons = buttons;
        shared->leftTrigger = xboxMode ? 255 : 0;
        shared->rightTrigger = 0;
        shared->leftThumbX = xboxMode ? thumbAxis(leftX) : 0;
        shared->leftThumbY = xboxMode ? thumbAxis(leftY) : 0;
        shared->rightThumbX = xboxMode ? thumbAxis(rightX) : 0;
        shared->rightThumbY = xboxMode ? thumbAxis(rightY) : 0;
        ++shared->packet;

        if ((buttons != 0 || xboxMode || std::abs(navX) > 0.1f || std::abs(navY) > 0.1f
                || (mRetailSurfaceXInputLogCount % 240) == 0)
            && mRetailSurfaceXInputLogCount < 480)
        {
            ++mRetailSurfaceXInputLogCount;
            Log(Debug::Info) << "FNVXR retail surface: published XInput buttons=0x" << std::hex << buttons
                             << std::dec << " packet=" << shared->packet
                             << " xboxMode=" << xboxMode
                             << " leftTrigger=" << static_cast<int>(shared->leftTrigger)
                             << " nav=(" << navX << "," << navY << ")";
        }
#endif
    }

    void VRInputManager::updateFallbackMovement(bool disableControls)
    {
        if (!VR::getVR())
        {
            sFallbackMovementInput = osg::Vec2f();
            sHasFallbackMovementInput = false;
            mFallbackMovementActive = false;
            return;
        }

        auto state = MWBase::Environment::get().getStateManager();
        auto window = MWBase::Environment::get().getWindowManager();
        auto world = MWBase::Environment::get().getWorld();
        auto lua = MWBase::Environment::get().getLuaManager();
        if (state->getState() == MWBase::StateManager::State_NoGame)
            return;

        const bool blocked = disableControls || window->isGuiMode() || FNVXRLiveFrameSurface::instance().modalInputActive();
        const std::string hand = VR::getLeftHandedMode() ? "/user/hand/right" : "/user/hand/left";
        auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
        const float side = blocked ? 0.f : vrMovementDeadzone(actionFloat(actionSet, {
            "/user/hand/left/input/thumbstick/x",
            "/user/hand/left/input/trackpad/x",
            "/user/hand/right/input/thumbstick/x",
            "/user/hand/right/input/trackpad/x",
        }));
        const float movement = blocked ? 0.f : vrMovementDeadzone(actionFloat(actionSet, {
            "/user/hand/left/input/thumbstick/y",
            "/user/hand/left/input/trackpad/y",
            "/user/hand/right/input/thumbstick/y",
            "/user/hand/right/input/trackpad/y",
        }));
        const bool active = std::abs(side) > 0.f || std::abs(movement) > 0.f;
        sFallbackMovementInput = osg::Vec2f(side, movement);
        sHasFallbackMovementInput = active;

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

        if ((active || mFallbackMovementActive) && mFallbackMovementLogCount < 32)
        {
            ++mFallbackMovementLogCount;
            Log(Debug::Verbose) << "FNV/ESM4 diag: VR fallback movement"
                             << " hand=" << hand << " side=" << side << " forward=" << movement
                             << " blocked=" << blocked << " active=" << active
                             << " settings=(" << movementSettings.mPosition[0] << ","
                             << movementSettings.mPosition[1] << "," << movementSettings.mPosition[2]
                             << ") speedFactor=" << movementSettings.mSpeedFactor;
        }

        mFallbackMovementActive = active;
    }

    void VRInputManager::updateRetailSurfaceModalAudio(bool active)
    {
        if (active == mRetailSurfaceAudioPaused)
            return;

        auto soundManager = MWBase::Environment::get().getSoundManager();
        if (active)
        {
            soundManager->pauseSounds(MWSound::BlockerType::VideoPlayback);
            mRetailSurfaceAudioPaused = true;
            Log(Debug::Info) << "FNVXR retail surface: OpenMW audio paused while retail surface is active";
        }
        else
        {
            soundManager->resumeSounds(MWSound::BlockerType::VideoPlayback);
            mRetailSurfaceAudioPaused = false;
            Log(Debug::Info) << "FNVXR retail surface: OpenMW audio resumed";
        }
    }

    void VRInputManager::updateRealisticCombat(float dt)
    {
        bool guiMode = MWBase::Environment::get().getWindowManager()->isGuiMode();

        if (!guiMode)
        {
            auto world = MWBase::Environment::get().getWorld();

            auto& player = world->getPlayer();
            auto playerPtr = world->getPlayerPtr();
            if (!mRealisticCombat || mRealisticCombat->ptr() != playerPtr)
            {
                mRealisticCombat.reset(new RealisticCombat::StateMachine(playerPtr, VR::getPreferredAimPath()));
            }
            bool enabled = !guiMode && player.getDrawState() == MWMechanics::DrawState::Weapon && !player.isDisabled();
            mRealisticCombat->update(dt, enabled);
        }
        else if (mRealisticCombat)
            mRealisticCombat->update(dt, false);

        auto ptr = MWBase::Environment::get().getWorld()->getPlayerPtr();
        auto* anim = MWBase::Environment::get().getWorld()->getAnimation(ptr);
        auto* vrAnim = static_cast<MWVR::VRAnimation*>(anim);
        mVRAimNode = vrAnim->getWeaponTransform();
    }

    void VRInputManager::updateVoidMessages()
    {
        auto wm = MWBase::Environment::get().getWindowManager();
        if (wm->isInteractiveMessageBoxActive())
            return;
        if (wm->isInVoid())
        {
            mVoidMessages.pop();
        }
        if (mVoidMessages.size() > 0)
        {
            wm->interactiveMessageBox(mVoidMessages.front(), { "#{Interface:OK}" });
            wm->enterVoid();
        }
        else
            wm->exitVoid();
    }

    MWWorld::Ptr VRInputManager::getPointerTarget() const
    {
        // TODO: In the future, dehardcode drag and drop as well
        if (!MWBase::Environment::get().getWindowManager()->getDragAndDrop().mIsOnDragAndDrop)
        {
            const auto& ray = mVRPointer->getPointerRay();
            if (ray.mHit)
            {
                return ray.mHitObject;
            }
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
        auto channel = mBindingsManager->ics().getChannel(MWInput::A_MoveLeftRight); // ->setValue(value);
        channel->setEnabled(true);
    }

    int VRInputManager::interactiveMessageBox(const std::string& message, const std::vector<std::string>& buttons)
    {
        auto wm = MWBase::Environment::get().getWindowManager();

        wm->enterVoid();

        wm->interactiveMessageBox(message, buttons, true);

        wm->exitVoid();

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
        if (injectMouseClickIfApplicable && MWVR::FNVXRLiveFrameSurface::instance().visible())
        {
            Log(Debug::Info) << "FNVXR retail surface: pointer activate visible=1 focus="
                             << MWVR::FNVXRLiveFrameSurface::instance().hasFocus();
            if (MWVR::FNVXRLiveFrameSurface::instance().injectMouseClick())
                return;
            return;
        }

        if (injectMouseClickIfApplicable && MWVR::VRGUIManager::instance().hasFocus())
        {
            if (MWVR::VRGUIManager::instance().injectMouseClick())
                return;

            SDL_MouseButtonEvent arg;
            mMouseManager->mousePressed(arg, SDL_BUTTON_LEFT);
            mMouseManager->mouseReleased(arg, SDL_BUTTON_LEFT);
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
        , mVRPointer(nullptr)
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

        auto wm = MWBase::Environment::get().getWindowManager();
        if (VR::getVR())
        {
            auto& actionSet = mXRInput->getActionSet(MWActionSet::Actions);
            if (actionSet.update())
                wm->skipVideo();
            updateRetailSurfaceGripRecenter(disableControls);
        }

        const bool retailSurfaceReady = retailSurfaceBridgeReady(disableControls);
        const bool retailSurfaceModal = retailSurfaceReady && MWVR::FNVXRLiveFrameSurface::instance().modalInputActive();
        updateRetailSurfaceModalAudio(retailSurfaceModal);
        if (retailSurfaceModal)
        {
            suppressOpenMWGuiForRetailSurface();
            rootPlayerMovementForRetailSurface();
        }
        updateVRPointer(disableControls);
        if (MWVR::FNVXRLiveFrameSurface::instance().visible())
            MWVR::FNVXRLiveFrameSurface::instance().updateAimPointer();
        updateDirectPointerClick();
        updateRetailSurfaceDirectClick(retailSurfaceReady);
        updateRetailSurfaceXInput(retailSurfaceReady);

        if (MWVR::VRGUIManager::instance().hasFocus())
        {
            auto guiCursor = MWVR::VRGUIManager::instance().guiCursor();
            mMouseManager->setMousePosition(guiCursor.x(), guiCursor.y());
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

                    mMouseManager->injectMouseMove(0.f, 0.f, std::floor(mScrollPoints), true);
                    mMouseManager->mouseWheelMoved(arg);
                    mScrollPoints -= std::floor(mScrollPoints);
                }
            }
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

        MWInput::InputManager::update(dt, disableControls || retailSurfaceModal, disableEvents || retailSurfaceModal);
        updateFallbackMovement(disableControls || disableEvents || retailSurfaceModal);

        if (retailSurfaceModal)
        {
            suppressOpenMWGuiForRetailSurface();
            rootPlayerMovementForRetailSurface();
        }

        updateVoidMessages();

        // The rest of this code assumes the game is running
        if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame)
            return;

        if (VR::getVR())
            updateVrDebugSnapshotControls();

        bool guiMode = wm->isGuiMode();

        // OpenMW assumes all input will come via SDL which i often violate.
        // This keeps player controls correctly enabled for my purposes.
        mBindingsManager->setPlayerControlsEnabled(!guiMode && !retailSurfaceModal);
        if (retailSurfaceModal)
            rootPlayerMovementForRetailSurface();
    }

    void VRInputManager::onSpaceUpdate()
    {
        if (VR::getVR()
            && MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_Running)
            updateRealisticCombat(mDt);
    }

}
