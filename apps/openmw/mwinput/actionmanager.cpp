#include "actionmanager.hpp"

#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>

#include <SDL_keyboard.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <vector>

#include <osg/Camera>
#include <osg/Matrix>
#include <osg/Viewport>
#include <osgViewer/Viewer>

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadbptd.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/misc/rng.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvplayerruntimestate.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/player.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "../mwgui/hud.hpp"

#include "../mwphysics/raycasting.hpp"

#include "../mwrender/animation.hpp"
#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"

#include "actions.hpp"
#include "bindingsmanager.hpp"

namespace MWInput
{

    ActionManager::ActionManager(BindingsManager* bindingsManager, osg::ref_ptr<osgViewer::Viewer> viewer,
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler)
        : mBindingsManager(bindingsManager)
        , mViewer(std::move(viewer))
        , mScreenCaptureHandler(std::move(screenCaptureHandler))
        , mTimeIdle(0.f)
    {
    }

    void ActionManager::update(float dt)
    {
        if (mBindingsManager->actionIsActive(A_MoveForward) || mBindingsManager->actionIsActive(A_MoveBackward)
            || mBindingsManager->actionIsActive(A_MoveLeft) || mBindingsManager->actionIsActive(A_MoveRight)
            || mBindingsManager->actionIsActive(A_Jump) || mBindingsManager->actionIsActive(A_Sneak)
            || mBindingsManager->actionIsActive(A_TogglePOV) || mBindingsManager->actionIsActive(A_ZoomIn)
            || mBindingsManager->actionIsActive(A_ZoomOut))
        {
            resetIdleTime();
        }
        else
            mTimeIdle += dt;

        if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Executing)
            updateFalloutVatsExecution(dt);
        else if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
            updateFalloutVatsPointerSelection();
    }

    void ActionManager::resetIdleTime()
    {
        mTimeIdle = 0.f;
    }

    void ActionManager::executeAction(int action)
    {
        MWBase::Environment::get().getLuaManager()->inputEvent({ MWBase::LuaManager::InputEvent::Action, action });
        const auto inputManager = MWBase::Environment::get().getInputManager();
        const auto windowManager = MWBase::Environment::get().getWindowManager();
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Inactive)
        {
            switch (action)
            {
                case A_QuickMenu:
                case A_GameMenu:
                case A_Screenshot:
                case A_Console:
                case A_Jump:
                case A_Activate:
                case A_Use:
                case A_MoveLeft:
                case A_MoveRight:
                case A_MoveForward:
                case A_MoveBackward:
                case A_ToggleHUD:
                case A_QuickSave:
                case A_QuickLoad:
                    break;
                default:
                    return;
            }
            if (action == A_GameMenu || action == A_Console
                || action == A_QuickSave || action == A_QuickLoad)
                cancelFalloutVats();
        }
        // trigger action activated
        switch (action)
        {
            case A_QuickMenu:
                if (isFalloutContent())
                    toggleFalloutVats();
                break;
            case A_GameMenu:
                toggleMainMenu();
                break;
            case A_Screenshot:
                screenshot();
                break;
            case A_Console:
                toggleConsole();
                break;
            case A_Jump:
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    executeFalloutVatsQueue();
                break;
            case A_Activate:
                inputManager->resetIdleTime();
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    executeFalloutVatsQueue();
                else if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Inactive)
                    activate();
                break;
            case A_Use:
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    queueFalloutVatsAttack();
                break;
            case A_ToggleSpell:
                if (isFalloutContent() && mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Inactive)
                    MWBase::Environment::get().getMechanicsManager()->reloadFalloutWeapon(
                        MWMechanics::getPlayer());
                break;
            case A_MoveLeft:
            case A_MoveRight:
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    cycleFalloutVatsTarget(action == A_MoveLeft ? -1 : 1);
                else if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Inactive)
                    handleGuiArrowKey(action);
                break;
            case A_MoveForward:
            case A_MoveBackward:
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    cycleFalloutVatsBodyPart(action == A_MoveForward ? -1 : 1);
                else if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Inactive)
                    handleGuiArrowKey(action);
                break;
            case A_Rest:
                rest();
                break;
            case A_QuickKey1:
                quickKey(1);
                break;
            case A_QuickKey2:
                quickKey(2);
                break;
            case A_QuickKey3:
                quickKey(3);
                break;
            case A_QuickKey4:
                quickKey(4);
                break;
            case A_QuickKey5:
                quickKey(5);
                break;
            case A_QuickKey6:
                quickKey(6);
                break;
            case A_QuickKey7:
                quickKey(7);
                break;
            case A_QuickKey8:
                quickKey(8);
                break;
            case A_QuickKey9:
                quickKey(9);
                break;
            case A_QuickKey10:
                quickKey(10);
                break;
            case A_ToggleHUD:
                windowManager->setHudVisibility(!windowManager->isHudVisible());
                break;
            case A_ToggleDebug:
                windowManager->toggleDebugWindow();
                break;
            case A_TogglePostProcessorHUD:
                windowManager->togglePostProcessorHud();
                break;
            case A_QuickSave:
                quickSave();
                break;
            case A_QuickLoad:
                quickLoad();
                break;
            case A_CycleSpellLeft:
                if (checkAllowedToUseItems() && windowManager->isAllowed(MWGui::GW_Magic))
                    MWBase::Environment::get().getWindowManager()->cycleSpell(false);
                break;
            case A_CycleSpellRight:
                if (checkAllowedToUseItems() && windowManager->isAllowed(MWGui::GW_Magic))
                    MWBase::Environment::get().getWindowManager()->cycleSpell(true);
                break;
            case A_CycleWeaponLeft:
                if (checkAllowedToUseItems() && windowManager->isAllowed(MWGui::GW_Inventory))
                    MWBase::Environment::get().getWindowManager()->cycleWeapon(false);
                break;
            case A_CycleWeaponRight:
                if (checkAllowedToUseItems() && windowManager->isAllowed(MWGui::GW_Inventory))
                    MWBase::Environment::get().getWindowManager()->cycleWeapon(true);
                break;
            case A_Inventory:
            case A_QuickKeysMenu:
                // Handled in Lua
                break;
            case A_Journal:
                if (isFalloutContent())
                {
                    if (!inputManager->getControlSwitch("playercontrols")
                        || !inputManager->getControlSwitch("playerinterface"))
                        break;
                    if (windowManager->containsMode(MWGui::GM_Inventory))
                        windowManager->removeGuiMode(MWGui::GM_Inventory);
                    else if (checkAllowedToUseItems() && windowManager->isAllowed(MWGui::GW_Inventory))
                    {
                        windowManager->pushGuiMode(MWGui::GM_Inventory);
                        windowManager->setActiveControllerWindow(MWGui::GM_Inventory, 2);
                    }
                }
                // Non-Fallout journal input remains handled by Lua.
                break;
            case A_Map:
                if (isFalloutContent())
                {
                    if (!inputManager->getControlSwitch("playercontrols")
                        || !inputManager->getControlSwitch("playerinterface"))
                        break;
                    if (windowManager->containsMode(MWGui::GM_Inventory))
                        windowManager->removeGuiMode(MWGui::GM_Inventory);
                    else if (checkAllowedToUseItems() && windowManager->isAllowed(MWGui::GW_Inventory))
                    {
                        windowManager->pushGuiMode(MWGui::GM_Inventory);
                        windowManager->setActiveControllerWindow(MWGui::GM_Inventory, 0);
                    }
                }
                break;
        }
    }

    bool ActionManager::checkAllowedToUseItems() const
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        if (player.getClass().getNpcStats(player).isWerewolf())
        {
            // Cannot use items or spells while in werewolf form
            MWBase::Environment::get().getWindowManager()->messageBox("#{sWerewolfRefusal}");
            return false;
        }
        return true;
    }

    bool ActionManager::isFalloutContent() const
    {
        const MWBase::World* world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return false;
        const MWWorld::ESM4Game game = world->getStore().getESM4Game();
        return game == MWWorld::ESM4Game::Fallout3 || game == MWWorld::ESM4Game::FalloutNewVegas;
    }

    void ActionManager::toggleFalloutVats()
    {
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Inactive)
        {
            cancelFalloutVats();
            return;
        }

        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        MWBase::InputManager* inputManager = MWBase::Environment::get().getInputManager();
        if (world == nullptr || windowManager == nullptr || inputManager == nullptr
            || windowManager->isGuiMode()
            || !inputManager->getControlSwitch("playercontrols")
            || !inputManager->getControlSwitch("playerfighting"))
            return;

        MWWorld::Ptr player = world->getPlayerPtr();
        if (player.isEmpty() || !player.getClass().isActor()
            || player.getClass().getCreatureStats(player).isDead())
            return;

        const auto validTarget = [&](const MWWorld::Ptr& candidate) {
            return !candidate.isEmpty() && candidate != player && candidate.getClass().isActor()
                && candidate.getRefData().isEnabled()
                && !candidate.getClass().getCreatureStats(candidate).isDead();
        };
        const auto screenTargetScore = [&](const MWWorld::Ptr& candidate) -> std::optional<float> {
            if (!validTarget(candidate) || mViewer == nullptr)
                return std::nullopt;
            const osg::Camera* renderCamera = mViewer->getCamera();
            const osg::Viewport* viewport = renderCamera != nullptr ? renderCamera->getViewport() : nullptr;
            if (renderCamera == nullptr || viewport == nullptr
                || viewport->width() <= 0.0 || viewport->height() <= 0.0)
                return std::nullopt;

            const osg::Vec3d window = osg::Vec3d(world->getActorHeadTransform(candidate).getTrans())
                * renderCamera->getViewMatrix() * renderCamera->getProjectionMatrix()
                * viewport->computeWindowMatrix();
            if (!std::isfinite(window.x()) || !std::isfinite(window.y()) || !std::isfinite(window.z())
                || window.z() < 0.0 || window.z() > 1.0)
                return std::nullopt;
            const float x = static_cast<float>((window.x() - viewport->x()) / viewport->width());
            const float y = static_cast<float>((window.y() - viewport->y()) / viewport->height());
            if (x < 0.f || x > 1.f || y < 0.f || y > 1.f)
                return std::nullopt;
            const float dx = x - 0.5f;
            const float dy = y - 0.5f;
            return dx * dx + dy * dy;
        };

        std::vector<MWWorld::Ptr> nearby;
        MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
            player.getRefData().getPosition().asVec3(), 3000.f, nearby);
        std::ranges::sort(nearby, [&](const MWWorld::Ptr& left, const MWWorld::Ptr& right) {
            const std::optional<float> leftScore = screenTargetScore(left);
            const std::optional<float> rightScore = screenTargetScore(right);
            if (leftScore.has_value() != rightScore.has_value())
                return leftScore.has_value();
            if (leftScore && rightScore && *leftScore != *rightScore)
                return *leftScore < *rightScore;
            const osg::Vec3f origin = player.getRefData().getPosition().asVec3();
            return (left.getRefData().getPosition().asVec3() - origin).length2()
                < (right.getRefData().getPosition().asVec3() - origin).length2();
        });

        mFalloutVatsTargets.clear();
        const MWWorld::Ptr faced = world->getFocusObject();
        if (validTarget(faced))
            mFalloutVatsTargets.push_back(faced);
        for (const MWWorld::Ptr& candidate : nearby)
        {
            if (screenTargetScore(candidate)
                && std::ranges::find(mFalloutVatsTargets, candidate) == mFalloutVatsTargets.end())
                mFalloutVatsTargets.push_back(candidate);
        }
        if (mFalloutVatsTargets.empty())
        {
            Log(Debug::Warning) << "FNV VATS: target acquisition failed";
            windowManager->messageBox("No V.A.T.S. target");
            return;
        }

        const std::optional<MWWorld::FalloutRuntimeActorValue> ap
            = world->getFalloutPlayerRuntimeState().getCurrentActorValue(
                MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue);
        if (!ap)
        {
            windowManager->messageBox("V.A.T.S. action points unavailable");
            return;
        }

        MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
        const MWWorld::ContainerStoreIterator weapon
            = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        MWMechanics::FalloutVatsWeaponFailure weaponFailure = MWMechanics::FalloutVatsWeaponFailure::None;
        if (weapon == inventory.end() || weapon->getType() != ESM4::Weapon::sRecordId
            || !(mFalloutVatsWeapon = MWMechanics::buildFalloutVatsWeaponContract(
                     *weapon->get<ESM4::Weapon>()->mBase, weaponFailure)))
        {
            Log(Debug::Warning) << "FNV VATS: equipped weapon contract unavailable reason="
                                << MWMechanics::getFalloutVatsWeaponFailureName(weaponFailure);
            windowManager->messageBox("Equipped weapon has no valid V.A.T.S. AP contract");
            return;
        }
        if (!mFalloutVats.enter(ap->mValue))
        {
            mFalloutVatsWeapon.reset();
            windowManager->messageBox("V.A.T.S. action points unavailable");
            return;
        }
        world->getFalloutPlayerRuntimeState().setVatsActive(true);

        player.getClass().getCreatureStats(player).setDrawState(MWMechanics::DrawState::Weapon);
        MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);

        MWRender::Camera* camera = world->getRenderingManager()->getCamera();
        mFalloutVatsPreviousCameraMode = static_cast<int>(camera->getMode());
        mFalloutVatsPreviousCameraDistance = camera->getCameraDistance();
        mFalloutVatsPreviousCameraPitch = camera->getPitch();
        mFalloutVatsPreviousCameraYaw = camera->getYaw();
        mFalloutVatsPreviousCameraRoll = camera->getRoll();
        mFalloutVatsPreviousSimulationScale = world->getTimeManager()->getSimulationTimeScale();
        world->getTimeManager()->setSimulationTimeScale(0.f);

        bool selected = false;
        for (std::size_t index = 0; index < mFalloutVatsTargets.size(); ++index)
        {
            if (!selectFalloutVatsTarget(mFalloutVatsTargets[index]))
                continue;
            mFalloutVatsTargetIndex = index;
            selected = true;
            break;
        }
        if (!selected)
        {
            mFalloutVats.cancel();
            restoreFalloutVatsView();
            windowManager->messageBox("No target has authored V.A.T.S. limbs");
        }
    }

    void ActionManager::cancelFalloutVats()
    {
        if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Inactive)
            return;
        if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Executing)
        {
            finishFalloutVatsExecution(true);
            return;
        }
        mFalloutVats.cancel();
        restoreFalloutVatsView();
        updateFalloutVatsHud();
    }

    bool ActionManager::selectFalloutVatsTarget(const MWWorld::Ptr& target)
    {
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Targeting
            || target.isEmpty() || !target.getClass().isActor()
            || target.getClass().getCreatureStats(target).isDead())
            return false;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        const ESM4::BodyPartData* targetBodyData = MWMechanics::getFalloutActorBodyPartData(target);
        if (targetBodyData == nullptr)
        {
            Log(Debug::Warning) << "FNV VATS: body data unavailable target="
                                << target.getClass().getName(target);
            return false;
        }

        std::vector<MWMechanics::FalloutVatsBodyPartContract> bodyParts;
        std::size_t selectedBodyPartIndex = 0;
        MWRender::Animation* animation = world->getAnimation(target);
        if (animation == nullptr)
            return false;
        for (std::size_t index = 0; index < targetBodyData->mBodyParts.size() && index <= 14; ++index)
        {
            MWMechanics::FalloutVatsBodyPartFailure bodyFailure
                = MWMechanics::FalloutVatsBodyPartFailure::None;
            const std::optional<MWMechanics::FalloutVatsBodyPartContract> candidate
                = MWMechanics::buildFalloutVatsBodyPartContract(
                    targetBodyData->mBodyParts[index], static_cast<std::uint8_t>(index), bodyFailure);
            if (!candidate || animation->getNode(candidate->mTargetNode) == nullptr)
                continue;
            if (candidate->mName == "Torso")
                selectedBodyPartIndex = bodyParts.size();
            bodyParts.push_back(*candidate);
        }
        if (bodyParts.empty())
            return false;

        clearFalloutVatsHighlight();
        mFalloutVatsTarget = target;
        mFalloutVatsTargetName = std::string(target.getClass().getName(target));
        mFalloutVatsBodyParts = std::move(bodyParts);
        mFalloutVatsBodyPartIndex = selectedBodyPartIndex;
        const float targetYaw = mFalloutVatsTarget.getRefData().getPosition().rot[2];
        mFalloutVatsTargetFramingForward = osg::Vec3f(std::sin(targetYaw), std::cos(targetYaw), 0.f);
        if (SceneUtil::PositionAttitudeTransform* baseNode = mFalloutVatsTarget.getRefData().getBaseNode())
            mFalloutVatsTargetFramingForward = baseNode->getAttitude() * osg::Vec3f(0.f, 1.f, 0.f);
        mFalloutVatsTargetFramingForward.z() = 0.f;
        if (mFalloutVatsTargetFramingForward.length2() < 0.001f)
            mFalloutVatsTargetFramingForward.set(0.f, 1.f, 0.f);
        else
            mFalloutVatsTargetFramingForward.normalize();
        mFalloutVatsExecutionCameraInitialized = false;
        updateFalloutVatsCamera();
        return selectFalloutVatsBodyPart(selectedBodyPartIndex);
    }

    bool ActionManager::selectFalloutVatsBodyPart(std::size_t index)
    {
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Targeting
            || mFalloutVatsTarget.isEmpty() || !mFalloutVatsWeapon
            || index >= mFalloutVatsBodyParts.size())
            return false;

        const MWMechanics::FalloutVatsBodyPartContract& bodyPart = mFalloutVatsBodyParts[index];
        const unsigned int hitChance
            = MWMechanics::getFalloutVatsDisplayedHitChance(bodyPart, *mFalloutVatsWeapon);
        if (!mFalloutVats.select(mFalloutVatsTarget.getCellRef().getRefNum(), bodyPart, hitChance))
            return false;

        mFalloutVatsBodyPartIndex = index;
        mFalloutVatsBodyPartName = std::string(bodyPart.mName);
        mFalloutVatsBodyPartTargetNode = std::string(bodyPart.mTargetNode);
        mFalloutVatsHitChance = hitChance;
        updateFalloutVatsHighlight();
        updateFalloutVatsHud();
        return true;
    }

    void ActionManager::cycleFalloutVatsBodyPart(int direction)
    {
        if (mFalloutVatsBodyParts.empty() || direction == 0)
            return;
        const std::size_t count = mFalloutVatsBodyParts.size();
        const std::size_t next = direction > 0
            ? (mFalloutVatsBodyPartIndex + 1) % count
            : (mFalloutVatsBodyPartIndex + count - 1) % count;
        selectFalloutVatsBodyPart(next);
    }

    void ActionManager::cycleFalloutVatsTarget(int direction)
    {
        if (mFalloutVatsTargets.size() < 2 || direction == 0)
            return;
        const std::size_t count = mFalloutVatsTargets.size();
        for (std::size_t offset = 1; offset < count; ++offset)
        {
            const std::size_t next = direction > 0
                ? (mFalloutVatsTargetIndex + offset) % count
                : (mFalloutVatsTargetIndex + count - offset) % count;
            if (!selectFalloutVatsTarget(mFalloutVatsTargets[next]))
                continue;
            mFalloutVatsTargetIndex = next;
            return;
        }
    }

    void ActionManager::updateFalloutVatsCamera()
    {
        if (mFalloutVatsTarget.isEmpty())
            return;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const osg::Vec3f head = world->getActorHeadTransform(mFalloutVatsTarget).getTrans();
        const osg::Vec3f base = mFalloutVatsTarget.getRefData().getPosition().asVec3();
        osg::Vec3f focus = head;
        float radius = 80.f;
        float actorHeight = head.z() - base.z();
        if (!std::isfinite(actorHeight) || actorHeight < 40.f || actorHeight > 300.f)
        {
            const osg::Vec3f halfExtents = world->getHalfExtents(mFalloutVatsTarget, true);
            const float extentHeight = halfExtents.z() * 2.f;
            actorHeight = std::isfinite(extentHeight) && extentHeight > 0.f
                ? std::clamp(extentHeight, 80.f, 260.f)
                : 128.f;
            focus = base + osg::Vec3f(0.f, 0.f, actorHeight * 0.8f);
        }
        if (std::isfinite(actorHeight) && actorHeight >= 40.f && actorHeight <= 300.f)
        {
            focus.z() -= actorHeight * 0.3f;
            radius = std::clamp(actorHeight * 0.55f, 70.f, 110.f);
        }

        MWMechanics::FalloutVatsCameraPose pose;
        if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Executing)
        {
            if (!mFalloutVatsExecutionCameraInitialized)
            {
                const MWWorld::Ptr player = world->getPlayerPtr();
                const osg::Vec3f playerHead = world->getActorHeadTransform(player).getTrans();
                osg::Vec3f shotAxis = head - playerHead;
                shotAxis.z() = 0.f;
                float shotDistance = shotAxis.length();
                if (!std::isfinite(shotDistance) || shotDistance < 1.f)
                {
                    shotAxis.set(0.f, 1.f, 0.f);
                    shotDistance = 160.f;
                }
                else
                    shotAxis /= shotDistance;

                if (mFalloutVatsExecutionCameraPhase == 0)
                {
                    const osg::Vec3f side(-shotAxis.y(), shotAxis.x(), 0.f);
                    mFalloutVatsExecutionCameraFocus
                        = playerHead + shotAxis * 15.f - osg::Vec3f(0.f, 0.f, 10.f);
                    osg::Vec3f candidate
                        = playerHead - shotAxis * 55.f + side * 70.f + osg::Vec3f(0.f, 0.f, 15.f);
                    MWPhysics::RayCastingResult cameraRay;
                    world->castRenderingRay(cameraRay, playerHead, candidate, true, true);
                    if (cameraRay.mHit)
                    {
                        const osg::Vec3f ray = candidate - playerHead;
                        const float length = ray.length();
                        if (length > 0.001f)
                            candidate = cameraRay.mHitPos - ray / length * 18.f;
                    }
                    mFalloutVatsExecutionCameraEye = candidate;
                }
                else
                {
                    const MWMechanics::FalloutVatsCameraPose targetPose
                        = MWMechanics::buildFalloutVatsFrontalCameraPose(
                            focus, radius, mFalloutVatsTargetFramingForward);
                    mFalloutVatsExecutionCameraEye = targetPose.mEye;
                    mFalloutVatsExecutionCameraFocus = targetPose.mFocus;
                }
                mFalloutVatsExecutionCameraInitialized = true;
            }
            pose.mEye = mFalloutVatsExecutionCameraEye;
            pose.mFocus = mFalloutVatsExecutionCameraFocus;
        }
        else
        {
            pose = MWMechanics::buildFalloutVatsFrontalCameraPose(
                focus, radius, mFalloutVatsTargetFramingForward);
        }

        const osg::Vec3d lookDirection = pose.mFocus - pose.mEye;
        const double horizontal
            = std::sqrt(lookDirection.x() * lookDirection.x() + lookDirection.y() * lookDirection.y());
        MWRender::Camera* camera = world->getRenderingManager()->getCamera();
        camera->setMode(MWRender::Camera::Mode::Static, true);
        camera->setStaticPosition(pose.mEye);
        camera->setPitch(static_cast<float>(std::atan2(lookDirection.z(), horizontal)), true);
        camera->setYaw(-static_cast<float>(std::atan2(lookDirection.x(), lookDirection.y())), true);
        camera->setRoll(0.f);
        camera->instantTransition();
        if (mViewer != nullptr && mViewer->getCamera() != nullptr)
            camera->updateCamera(mViewer->getCamera());
    }

    void ActionManager::updateFalloutVatsHighlight()
    {
        if (mFalloutVatsTarget.isEmpty())
            return;
        MWRender::Animation* animation
            = MWBase::Environment::get().getWorld()->getAnimation(mFalloutVatsTarget);
        if (animation == nullptr)
            return;

        std::vector<std::string_view> targetNodes;
        targetNodes.reserve(mFalloutVatsBodyParts.size());
        for (const MWMechanics::FalloutVatsBodyPartContract& bodyPart : mFalloutVatsBodyParts)
            targetNodes.push_back(bodyPart.mTargetNode);
        animation->setFalloutVatsWireframes(targetNodes, mFalloutVatsBodyPartTargetNode, true);
    }

    void ActionManager::clearFalloutVatsHighlight()
    {
        if (mFalloutVatsTarget.isEmpty())
            return;
        if (MWRender::Animation* animation
            = MWBase::Environment::get().getWorld()->getAnimation(mFalloutVatsTarget))
            animation->setFalloutVatsWireframes({}, {}, false);
    }

    void ActionManager::restoreFalloutVatsView()
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        clearFalloutVatsHighlight();
        world->getFalloutPlayerRuntimeState().setVatsActive(false);
        if (mFalloutVatsPreviousCameraMode >= 0)
        {
            world->getTimeManager()->setSimulationTimeScale(mFalloutVatsPreviousSimulationScale);
            MWRender::Camera* camera = world->getRenderingManager()->getCamera();
            camera->attachTo(world->getPlayerPtr());
            camera->setPreferredCameraDistance(mFalloutVatsPreviousCameraDistance);
            camera->setMode(static_cast<MWRender::Camera::Mode>(mFalloutVatsPreviousCameraMode), true);
            camera->setPitch(mFalloutVatsPreviousCameraPitch, true);
            camera->setYaw(mFalloutVatsPreviousCameraYaw, true);
            camera->setRoll(mFalloutVatsPreviousCameraRoll);
            camera->instantTransition();
            camera->processViewChange();
        }

        mFalloutVatsWeapon.reset();
        mFalloutVatsTarget = {};
        mFalloutVatsTargets.clear();
        mFalloutVatsTargetIndex = 0;
        mFalloutVatsBodyParts.clear();
        mFalloutVatsBodyPartIndex = 0;
        mFalloutVatsTargetName.clear();
        mFalloutVatsBodyPartName.clear();
        mFalloutVatsBodyPartTargetNode.clear();
        mFalloutVatsHitChance = 0;
        mFalloutVatsPreviousCameraMode = -1;
        mFalloutVatsPreviousCameraDistance = 0.f;
        mFalloutVatsPreviousCameraPitch = 0.f;
        mFalloutVatsPreviousCameraYaw = 0.f;
        mFalloutVatsPreviousCameraRoll = 0.f;
        mFalloutVatsPreviousPlayerYaw = 0.f;
        mFalloutVatsPlayerYawChanged = false;
        mFalloutVatsPreviousSimulationScale = 1.f;
        mFalloutVatsTargetFramingForward.set(0.f, 1.f, 0.f);
        mFalloutVatsExecutionCameraEye.set(0.f, 0.f, 0.f);
        mFalloutVatsExecutionCameraFocus.set(0.f, 0.f, 0.f);
        mFalloutVatsExecutionCameraInitialized = false;
        mFalloutVatsExecutionCameraPhase = 0;
        mFalloutVatsExecutionTimer = 0.f;
        mFalloutVatsExecutionApBefore = 0.f;
        mFalloutVatsExecutionPlannedApAfter = 0.f;
        mFalloutVatsExecutionApSpent = 0.f;
        mFalloutVatsExecutionDamage = 0.f;
        mFalloutVatsExecutionQueued = 0;
        mFalloutVatsExecutionShotsAttempted = 0;
        mFalloutVatsExecutionShotsFired = 0;
        mFalloutVatsExecutionRolledHits = 0;
        mFalloutVatsExecutionVisualPrepared = false;
        mFalloutVatsExecutionTargetHealthBefore.clear();
    }

    void ActionManager::queueFalloutVatsAttack()
    {
        if (!mFalloutVatsWeapon)
            return;
        MWMechanics::FalloutVatsQueueFailure failure = MWMechanics::FalloutVatsQueueFailure::None;
        if (!mFalloutVats.queueSelected(
                *mFalloutVatsWeapon, getFalloutVatsAvailableShots(), failure))
        {
            MWBase::Environment::get().getWindowManager()->messageBox(
                std::string("V.A.T.S. queue failed: ")
                + std::string(MWMechanics::getFalloutVatsQueueFailureName(failure)));
        }
        updateFalloutVatsHud();
    }

    std::size_t ActionManager::getFalloutVatsAvailableShots() const
    {
        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        const MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
        const MWWorld::ConstContainerStoreIterator weapon
            = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if (weapon == inventory.end() || weapon->getType() != ESM4::Weapon::sRecordId)
            return 0;

        const ESM4::Weapon& base = *weapon->get<ESM4::Weapon>()->mBase;
        if (MWMechanics::isFalloutThrownWeapon(base))
        {
            return static_cast<std::size_t>(
                std::max(0, inventory.count(ESM::RefId::formIdRefId(base.mId))));
        }
        if (base.mData.ammoUse == 0)
            return 0;

        const ESM::RefId weaponId = ESM::RefId::formIdRefId(base.mId);
        const int capacity = std::max<int>(base.mData.clipSize, base.mData.ammoUse);
        if (const std::optional<int> loaded = inventory.getFalloutLoadedAmmo(weaponId))
        {
            if (*loaded < 0 || *loaded > capacity)
                return 0;
            return static_cast<std::size_t>(*loaded) / base.mData.ammoUse;
        }

        const MWWorld::ESMStore& store = MWBase::Environment::get().getWorld()->getStore();
        std::vector<ESM::FormId> candidates;
        if (store.get<ESM4::Ammunition>().search(base.mAmmo) != nullptr)
            candidates.push_back(base.mAmmo);
        else if (const ESM4::FormIdList* list = store.get<ESM4::FormIdList>().search(base.mAmmo))
            candidates = list->mObjects;
        else
            return 0;

        for (ESM::FormId candidate : candidates)
        {
            if (candidate.isZeroOrUnset()
                || store.get<ESM4::Ammunition>().search(candidate) == nullptr)
                continue;
            const int reserve = inventory.count(ESM::RefId::formIdRefId(candidate));
            if (reserve >= base.mData.ammoUse)
            {
                const int initialMagazine = std::min(capacity, reserve);
                return static_cast<std::size_t>(initialMagazine) / base.mData.ammoUse;
            }
        }
        return 0;
    }

    void ActionManager::executeFalloutVatsQueue()
    {
        const std::optional<float> plannedApAfter = mFalloutVats.beginExecution();
        if (!plannedApAfter || mFalloutVatsTarget.isEmpty())
            return;

        mFalloutVatsExecutionTimer = 0.f;
        mFalloutVatsExecutionApBefore = mFalloutVats.getActionPointsBefore();
        mFalloutVatsExecutionPlannedApAfter = *plannedApAfter;
        mFalloutVatsExecutionApSpent = 0.f;
        mFalloutVatsExecutionDamage = 0.f;
        mFalloutVatsExecutionQueued = mFalloutVats.getQueue().size();
        mFalloutVatsExecutionShotsAttempted = 0;
        mFalloutVatsExecutionShotsFired = 0;
        mFalloutVatsExecutionRolledHits = 0;
        mFalloutVatsExecutionTargetHealthBefore.clear();
        mFalloutVatsExecutionVisualPrepared = false;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        world->getTimeManager()->setSimulationTimeScale(0.2f);
        const MWWorld::Ptr player = world->getPlayerPtr();
        const ESM::Position& playerPosition = player.getRefData().getPosition();
        mFalloutVatsPreviousPlayerYaw = playerPosition.rot[2];
        osg::Vec3f toTarget = world->getActorHeadTransform(mFalloutVatsTarget).getTrans()
            - world->getActorHeadTransform(player).getTrans();
        toTarget.z() = 0.f;
        if (toTarget.length2() > 0.001f)
        {
            const float targetYaw = std::atan2(toTarget.x(), toTarget.y());
            world->rotateObject(player,
                osg::Vec3f(playerPosition.rot[0], playerPosition.rot[1], targetYaw),
                MWBase::RotationFlag_none);
            mFalloutVatsPlayerYawChanged = true;
        }
        mFalloutVatsExecutionCameraPhase = 0;
        mFalloutVatsExecutionCameraInitialized = false;
        updateFalloutVatsHud();
    }

    void ActionManager::updateFalloutVatsExecution(float dt)
    {
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Executing)
            return;
        if (std::isfinite(dt) && dt > 0.f)
            mFalloutVatsExecutionTimer += dt;

        if (mFalloutVats.getExecutingAction() != nullptr)
        {
            if (!mFalloutVatsExecutionVisualPrepared)
            {
                if (mFalloutVatsExecutionShotsAttempted > 0 && mFalloutVatsExecutionTimer < 0.35f)
                {
                    if (mFalloutVatsExecutionTimer >= 0.12f && mFalloutVatsExecutionCameraPhase == 0)
                    {
                        mFalloutVatsExecutionCameraPhase = 1;
                        mFalloutVatsExecutionCameraInitialized = false;
                    }
                    updateFalloutVatsCamera();
                    updateFalloutVatsHud();
                    return;
                }
                if (!MWBase::Environment::get().getMechanicsManager()->prepareFalloutVatsRangedAttack(
                        MWBase::Environment::get().getWorld()->getPlayerPtr()))
                {
                    Log(Debug::Error) << "FNV VATS execution: authored visual wind-up unavailable";
                    finishFalloutVatsExecution(true);
                    return;
                }
                mFalloutVatsExecutionVisualPrepared = true;
                mFalloutVatsExecutionCameraPhase = 0;
                mFalloutVatsExecutionCameraInitialized = false;
                mFalloutVatsExecutionTimer = 0.f;
            }
            updateFalloutVatsCamera();
            updateFalloutVatsHud();

            if (mFalloutVatsExecutionTimer < 0.1f)
                return;
            if (!MWBase::Environment::get().getMechanicsManager()->consumeFalloutVatsRangedAttackRelease(
                    MWBase::Environment::get().getWorld()->getPlayerPtr()))
            {
                if (mFalloutVatsExecutionTimer < 2.f)
                    return;
                Log(Debug::Error) << "FNV VATS execution: authored release key timed out";
                finishFalloutVatsExecution(true);
                return;
            }

            mFalloutVatsExecutionTimer = 0.f;
            if (!executeNextFalloutVatsAction())
            {
                finishFalloutVatsExecution(true);
                return;
            }
            mFalloutVatsExecutionVisualPrepared = false;
            return;
        }

        if (mFalloutVatsExecutionShotsAttempted > 0 && mFalloutVatsExecutionTimer >= 0.12f
            && mFalloutVatsExecutionCameraPhase == 0)
        {
            mFalloutVatsExecutionCameraPhase = 1;
            mFalloutVatsExecutionCameraInitialized = false;
        }
        updateFalloutVatsCamera();
        updateFalloutVatsHud();

        if (mFalloutVats.isExecutionComplete() && mFalloutVatsExecutionTimer >= 0.9f
            && MWBase::Environment::get().getWorld()->countPendingFalloutVatsProjectiles(
                MWBase::Environment::get().getWorld()->getPlayerPtr()) == 0)
            finishFalloutVatsExecution(false);
    }

    bool ActionManager::executeNextFalloutVatsAction()
    {
        const MWMechanics::FalloutVatsQueuedAction* queued = mFalloutVats.getExecutingAction();
        if (queued == nullptr)
            return false;
        const MWMechanics::FalloutVatsQueuedAction executing = *queued;
        ++mFalloutVatsExecutionShotsAttempted;

        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr executionTarget;
        const auto target = std::ranges::find_if(mFalloutVatsTargets,
            [&](const MWWorld::Ptr& candidate) {
                return !candidate.isEmpty() && candidate.getCellRef().getRefNum() == executing.mTarget;
            });
        if (target != mFalloutVatsTargets.end())
            executionTarget = *target;
        else
            executionTarget = world->searchPtr(ESM::RefId(executing.mTarget), true, false);
        if (executionTarget.isEmpty() || !executionTarget.getClass().isActor())
            return false;

        if (executionTarget != mFalloutVatsTarget)
        {
            mFalloutVatsTarget = executionTarget;
            mFalloutVatsTargetName = std::string(executionTarget.getClass().getName(executionTarget));
        }
        mFalloutVatsBodyPartName = executing.mBodyPartName;
        mFalloutVatsBodyPartTargetNode = executing.mTargetNode;
        mFalloutVatsHitChance = executing.mDisplayedHitChance;
        updateFalloutVatsHighlight();
        updateFalloutVatsCamera();
        updateFalloutVatsHud();

        const float healthBefore
            = executionTarget.getClass().getCreatureStats(executionTarget).getHealth().getCurrent();
        if (std::ranges::none_of(mFalloutVatsExecutionTargetHealthBefore,
                [&](const auto& entry) { return entry.first == executing.mTarget; }))
            mFalloutVatsExecutionTargetHealthBefore.emplace_back(executing.mTarget, healthBefore);

        osg::Vec3f targetPoint = world->getActorHeadTransform(executionTarget).getTrans();
        if (const std::optional<osg::Matrixf> transform
            = world->getActorNodeTransform(executionTarget, executing.mTargetNode))
            targetPoint = transform->getTrans();

        const float hitRoll = Misc::Rng::rollProbability(world->getPrng());
        const bool rolledHit
            = MWMechanics::doesFalloutVatsAttackHit(executing.mDisplayedHitChance, hitRoll);
        const bool fired = MWBase::Environment::get().getMechanicsManager()->executeFalloutVatsRangedHit(
            world->getPlayerPtr(), executionTarget, targetPoint, executing, rolledHit);
        const float healthAfter
            = executionTarget.getClass().getCreatureStats(executionTarget).getHealth().getCurrent();
        mFalloutVatsExecutionDamage += std::max(0.f, healthBefore - healthAfter);
        Log(fired ? Debug::Info : Debug::Error)
            << "FNV VATS action: target=" << executionTarget.getClass().getName(executionTarget)
            << " bodyPart=" << executing.mBodyPartName
            << " hitChance=" << static_cast<unsigned int>(executing.mDisplayedHitChance)
            << " roll=" << hitRoll << " rolledHit=" << rolledHit << " fired=" << fired;
        if (!fired)
            return false;

        ++mFalloutVatsExecutionShotsFired;
        if (rolledHit)
            ++mFalloutVatsExecutionRolledHits;
        mFalloutVatsExecutionApSpent += executing.mActionPointCost;
        const float apAfter
            = std::max(0.f, mFalloutVatsExecutionApBefore - mFalloutVatsExecutionApSpent);
        world->getFalloutPlayerRuntimeState().setCurrentActorValue(
            MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue, apAfter);
        return mFalloutVats.advanceExecution();
    }

    void ActionManager::finishFalloutVatsExecution(bool interrupted)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        float observedDamage = 0.f;
        for (const auto& [targetId, healthBefore] : mFalloutVatsExecutionTargetHealthBefore)
        {
            MWWorld::Ptr target;
            const auto activeTarget = std::ranges::find_if(mFalloutVatsTargets,
                [&](const MWWorld::Ptr& candidate) {
                    return !candidate.isEmpty() && candidate.getCellRef().getRefNum() == targetId;
                });
            if (activeTarget != mFalloutVatsTargets.end())
                target = *activeTarget;
            else
                target = world->searchPtr(ESM::RefId(targetId), true, false);
            if (target.isEmpty() || !target.getClass().isActor())
                continue;
            const float healthAfter = target.getClass().getCreatureStats(target).getHealth().getCurrent();
            if (std::isfinite(healthAfter))
                observedDamage += std::max(0.f, healthBefore - healthAfter);
        }
        mFalloutVatsExecutionDamage = std::max(mFalloutVatsExecutionDamage, observedDamage);
        const float apAfter
            = std::max(0.f, mFalloutVatsExecutionApBefore - mFalloutVatsExecutionApSpent);
        world->getFalloutPlayerRuntimeState().setCurrentActorValue(
            MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue, apAfter);
        Log(interrupted ? Debug::Warning : Debug::Info)
            << "FNV VATS execution: phase=end interrupted=" << interrupted
            << " queued=" << mFalloutVatsExecutionQueued
            << " shotsAttempted=" << mFalloutVatsExecutionShotsAttempted
            << " shotsFired=" << mFalloutVatsExecutionShotsFired
            << " rolledHits=" << mFalloutVatsExecutionRolledHits
            << " apBefore=" << mFalloutVatsExecutionApBefore
            << " plannedApAfter=" << mFalloutVatsExecutionPlannedApAfter
            << " apAfter=" << apAfter << " totalDamage=" << mFalloutVatsExecutionDamage;

        if (!interrupted && !mFalloutVats.finishExecution())
            mFalloutVats.cancel();
        else if (interrupted)
            mFalloutVats.cancel();

        if (mFalloutVatsPlayerYawChanged)
        {
            const MWWorld::Ptr player = world->getPlayerPtr();
            const ESM::Position& position = player.getRefData().getPosition();
            world->rotateObject(player,
                osg::Vec3f(position.rot[0], position.rot[1], mFalloutVatsPreviousPlayerYaw),
                MWBase::RotationFlag_none);
            mFalloutVatsPlayerYawChanged = false;
        }
        restoreFalloutVatsView();
        updateFalloutVatsHud();
    }

    void ActionManager::updateFalloutVatsPointerSelection()
    {
        if (mFalloutVatsTarget.isEmpty() || mFalloutVatsBodyParts.empty() || mViewer == nullptr)
            return;

        const osg::Camera* camera = mViewer->getCamera();
        const osg::Viewport* viewport = camera != nullptr ? camera->getViewport() : nullptr;
        if (camera == nullptr || viewport == nullptr
            || viewport->width() <= 0.0 || viewport->height() <= 0.0)
            return;

        const MyGUI::IntSize guiSize = MyGUI::RenderManager::getInstance().getViewSize();
        if (guiSize.width <= 0 || guiSize.height <= 0)
            return;
        const MyGUI::IntPoint mouse = MyGUI::InputManager::getInstance().getMousePosition();
        const float mouseX = static_cast<float>(mouse.left) / static_cast<float>(guiSize.width);
        const float mouseY = static_cast<float>(mouse.top) / static_cast<float>(guiSize.height);

        MWBase::World* world = MWBase::Environment::get().getWorld();
        std::optional<std::size_t> closest;
        float closestDistanceSquared = std::numeric_limits<float>::max();
        for (std::size_t index = 0; index < mFalloutVatsBodyParts.size(); ++index)
        {
            const std::optional<osg::Matrixf> transform
                = world->getActorNodeTransform(
                    mFalloutVatsTarget, mFalloutVatsBodyParts[index].mTargetNode);
            if (!transform)
                continue;
            const osg::Vec3d window = osg::Vec3d(transform->getTrans()) * camera->getViewMatrix()
                * camera->getProjectionMatrix() * viewport->computeWindowMatrix();
            if (!std::isfinite(window.x()) || !std::isfinite(window.y())
                || window.z() < 0.0 || window.z() > 1.0)
                continue;
            const float x = static_cast<float>((window.x() - viewport->x()) / viewport->width());
            const float y = 1.f - static_cast<float>((window.y() - viewport->y()) / viewport->height());
            const float dx = (x - mouseX) * static_cast<float>(guiSize.width);
            const float dy = (y - mouseY) * static_cast<float>(guiSize.height);
            const float distanceSquared = dx * dx + dy * dy;
            if (distanceSquared < closestDistanceSquared)
            {
                closest = index;
                closestDistanceSquared = distanceSquared;
            }
        }

        constexpr float selectionRadius = 56.f;
        if (closest && closestDistanceSquared <= selectionRadius * selectionRadius
            && *closest != mFalloutVatsBodyPartIndex)
            selectFalloutVatsBodyPart(*closest);
    }

    void ActionManager::updateFalloutVatsHud()
    {
        MWGui::HUD* hud = MWBase::Environment::get().getWindowManager()->getHud();
        if (hud == nullptr)
            return;
        const bool visible
            = mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Inactive;
        std::vector<MWGui::FalloutVatsBodyPartDisplay> bodyPartDisplays;
        if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting
            && !mFalloutVatsTarget.isEmpty() && mFalloutVatsWeapon && mViewer != nullptr)
        {
            const osg::Camera* renderCamera = mViewer->getCamera();
            const osg::Viewport* viewport
                = renderCamera != nullptr ? renderCamera->getViewport() : nullptr;
            if (renderCamera != nullptr && viewport != nullptr
                && viewport->width() > 0.0 && viewport->height() > 0.0)
            {
                MWBase::World* world = MWBase::Environment::get().getWorld();
                bodyPartDisplays.reserve(mFalloutVatsBodyParts.size());
                for (std::size_t index = 0; index < mFalloutVatsBodyParts.size(); ++index)
                {
                    const MWMechanics::FalloutVatsBodyPartContract& bodyPart
                        = mFalloutVatsBodyParts[index];
                    const std::optional<osg::Matrixf> transform
                        = world->getActorNodeTransform(mFalloutVatsTarget, bodyPart.mTargetNode);
                    if (!transform)
                        continue;
                    const osg::Vec3d window = osg::Vec3d(transform->getTrans())
                        * renderCamera->getViewMatrix() * renderCamera->getProjectionMatrix()
                        * viewport->computeWindowMatrix();
                    const float viewportX
                        = static_cast<float>((window.x() - viewport->x()) / viewport->width());
                    const float viewportY
                        = 1.f - static_cast<float>((window.y() - viewport->y()) / viewport->height());
                    if (!std::isfinite(viewportX) || !std::isfinite(viewportY))
                        continue;
                    bodyPartDisplays.push_back({ bodyPart.mName,
                        MWMechanics::getFalloutVatsDisplayedHitChance(
                            bodyPart, *mFalloutVatsWeapon),
                        viewportX, viewportY, index == mFalloutVatsBodyPartIndex });
                }
            }
        }
        hud->setFalloutVatsVisible(visible, mFalloutVatsTargetName, bodyPartDisplays,
            mFalloutVats.getActionPointsBefore(), mFalloutVats.getActionPointsAfter(),
            mFalloutVats.getQueue().size(), visible ? getFalloutVatsAvailableShots() : 0,
            mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Executing);
    }

    void ActionManager::screenshot()
    {
        mScreenCaptureHandler->setFramesToCapture(1);
        mScreenCaptureHandler->captureNextFrame(*mViewer);
    }

    void ActionManager::toggleMainMenu()
    {
        if (MyGUI::InputManager::getInstance().isModalAny())
        {
            MWBase::Environment::get().getWindowManager()->exitCurrentModal();
            return;
        }

        if (MWBase::Environment::get().getWindowManager()->isConsoleMode())
        {
            MWBase::Environment::get().getWindowManager()->toggleConsole();
            return;
        }

        if (MWBase::Environment::get().getWindowManager()->isPostProcessorHudVisible())
        {
            MWBase::Environment::get().getWindowManager()->togglePostProcessorHud();
            return;
        }

        if (!MWBase::Environment::get().getWindowManager()->isGuiMode()) // No open GUIs, open up the MainMenu
        {
            MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_MainMenu);
        }
        else // Close current GUI
        {
            MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
        }
    }

    void ActionManager::quickLoad()
    {
        if (!MyGUI::InputManager::getInstance().isModalAny())
            MWBase::Environment::get().getStateManager()->quickLoad();
    }

    void ActionManager::quickSave()
    {
        if (!MyGUI::InputManager::getInstance().isModalAny())
            MWBase::Environment::get().getStateManager()->quickSave();
    }

    void ActionManager::rest()
    {
        if (!MWBase::Environment::get().getInputManager()->getControlSwitch("playercontrols")
            || !MWBase::Environment::get().getInputManager()->getControlSwitch("playerinterface"))
            return;

        if (!MWBase::Environment::get().getWindowManager()->getRestEnabled()
            || MWBase::Environment::get().getWindowManager()->isGuiMode())
            return;

        MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_Rest); // Open rest GUI
    }

    void ActionManager::toggleConsole()
    {
        if (MyGUI::InputManager::getInstance().isModalAny())
            return;

        MWBase::Environment::get().getWindowManager()->toggleConsole();
    }

    void ActionManager::quickKey(int index)
    {
        if (!MWBase::Environment::get().getInputManager()->getControlSwitch("playercontrols")
            || !MWBase::Environment::get().getInputManager()->getControlSwitch("playerfighting")
            || !MWBase::Environment::get().getInputManager()->getControlSwitch("playermagic"))
            return;
        if (!checkAllowedToUseItems())
            return;

        if (MWBase::Environment::get().getWorld()->getGlobalFloat(MWWorld::Globals::sCharGenState) != -1)
            return;

        if (!MWBase::Environment::get().getWindowManager()->isGuiMode())
            MWBase::Environment::get().getWindowManager()->activateQuickKey(index);
    }

    void ActionManager::activate()
    {
        if (MWBase::Environment::get().getWindowManager()->isGuiMode())
        {
            bool joystickUsed = MWBase::Environment::get().getInputManager()->joystickLastUsed();
            if (!SDL_IsTextInputActive() && !mBindingsManager->isLeftOrRightButton(A_Activate, joystickUsed))
                MWBase::Environment::get().getWindowManager()->injectKeyPress(MyGUI::KeyCode::Return, 0, false);
        }
        else if (MWBase::Environment::get().getInputManager()->getControlSwitch("playercontrols")
            && MWBase::Environment::get().getInputManager()->getControlSwitch("playermovement"))
        {
            MWWorld::Player& player = MWBase::Environment::get().getWorld()->getPlayer();
            player.activate();
        }
    }

    bool ActionManager::isSneaking() const
    {
        const MWBase::Environment& env = MWBase::Environment::get();
        return env.getMechanicsManager()->isSneaking(env.getWorld()->getPlayer().getPlayer());
    }

    void ActionManager::handleGuiArrowKey(int action)
    {
        bool joystickUsed = MWBase::Environment::get().getInputManager()->joystickLastUsed();
        // This is currently keyboard-specific code
        // TODO: see if GUI controls can be refactored into a single function
        if (joystickUsed)
            return;

        if (SDL_IsTextInputActive())
            return;

        if (mBindingsManager->isLeftOrRightButton(action, joystickUsed))
            return;

        MyGUI::KeyCode key;
        switch (action)
        {
            case A_MoveLeft:
                key = MyGUI::KeyCode::ArrowLeft;
                break;
            case A_MoveRight:
                key = MyGUI::KeyCode::ArrowRight;
                break;
            case A_MoveForward:
                key = MyGUI::KeyCode::ArrowUp;
                break;
            case A_MoveBackward:
            default:
                key = MyGUI::KeyCode::ArrowDown;
                break;
        }

        MWBase::Environment::get().getWindowManager()->injectKeyPress(key, 0, false);
    }
}
