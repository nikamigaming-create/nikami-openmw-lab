#include "actionmanager.hpp"

#include <MyGUI_InputManager.h>

#include <SDL_keyboard.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ranges>
#include <vector>

#include <components/settings/values.hpp>
#include <components/debug/debuglog.hpp>
#include <components/misc/rng.hpp>
//## VR_PATCH BEGIN
#include <components/vr/vr.hpp>
//## VR_PATCH END

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvplayerruntimestate.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/player.hpp"

#include <components/esm4/loadbptd.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadweap.hpp>

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "../mwrender/animation.hpp"
#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"

#include "../mwgui/hud.hpp"

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

        if (std::getenv("OPENMW_FNV_VATS_CAPTURE") != nullptr && !mFalloutVatsCaptureQueued
            && isFalloutContent())
        {
            const bool videoCapture = std::getenv("OPENMW_FNV_VATS_VIDEO") != nullptr;
            if (!mFalloutVatsCapturePrepared)
            {
                MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
                MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
                MWWorld::ContainerStoreIterator weapon
                    = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                if (weapon == inventory.end() || weapon->getType() != ESM4::Weapon::sRecordId)
                {
                    weapon = static_cast<MWWorld::ContainerStore&>(inventory).add(
                        ESM::RefId(ESM::FormId::fromUint32(0x0107ea24)), 1, false);
                    inventory.equip(MWWorld::InventoryStore::Slot_CarriedRight, weapon);
                    player.getClass().getCreatureStats(player).setDrawState(MWMechanics::DrawState::Weapon);
                }
                Log(Debug::Info) << "FNV VATS capture: prepared player weapon="
                                 << weapon->getClass().getName(*weapon);
                mFalloutVatsCapturePrepared = true;
            }
            mFalloutVatsCaptureTimer += dt;
            ++mFalloutVatsCaptureFrames;
            if (!mFalloutVatsTarget.isEmpty()
                && mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
            {
                const osg::Vec3f targetPosition = mFalloutVatsTarget.getRefData().getPosition().asVec3();
                const float targetYaw = mFalloutVatsTarget.getRefData().getPosition().rot[2];
                const osg::Vec3d focus(targetPosition.x(), targetPosition.y(), targetPosition.z() + 64.f);
                const osg::Vec3d eye(targetPosition.x() - std::cos(targetYaw) * 150.f,
                    targetPosition.y() + std::sin(targetYaw) * 150.f, targetPosition.z() + 64.f);
                MWRender::Camera* camera = MWBase::Environment::get().getWorld()
                                               ->getRenderingManager()
                                               ->getCamera();
                camera->setStaticPosition(eye);
                const osg::Vec3d lookDirection = focus - eye;
                camera->setYaw(
                    -static_cast<float>(std::atan2(lookDirection.x(), lookDirection.y())), true);
                camera->updateCamera();
            }
            if ((mFalloutVatsCaptureTimer >= 2.f || mFalloutVatsCaptureFrames >= 30)
                && !mFalloutVatsVideoExited
                && mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Inactive)
                toggleFalloutVats();
            if ((mFalloutVatsCaptureTimer >= 2.5f || mFalloutVatsCaptureFrames >= 45)
                && mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting
                && mFalloutVats.getQueue().empty())
                queueFalloutVatsAttack();
            if (videoCapture && mFalloutVatsCaptureFrames == 90
                && mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting
                && mFalloutVats.getQueue().size() == 1)
                queueFalloutVatsAttack();
            if (videoCapture && mFalloutVatsCaptureFrames == 150
                && mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
            {
                toggleFalloutVats();
                MWRender::Camera* camera = MWBase::Environment::get().getWorld()
                                               ->getRenderingManager()
                                               ->getCamera();
                camera->attachTo(MWBase::Environment::get().getWorld()->getPlayerPtr());
                camera->setMode(MWRender::Camera::Mode::FirstPerson, true);
                camera->processViewChange();
                mFalloutVatsVideoExited = true;
            }
            if (videoCapture && mFalloutVatsCaptureFrames >= 60 && mFalloutVatsCaptureFrames <= 180
                && mFalloutVatsCaptureFrames % 6 == 0)
            {
                screenshot();
                ++mFalloutVatsVideoCaptureCount;
            }
            if (videoCapture && mFalloutVatsCaptureFrames > 180)
            {
                Log(Debug::Info) << "FNV VATS capture: video complete frames=" << mFalloutVatsVideoCaptureCount;
                mFalloutVatsCaptureQueued = true;
            }
            else if (!videoCapture && (mFalloutVatsCaptureTimer >= 3.f || mFalloutVatsCaptureFrames >= 60)
                && !mFalloutVats.getQueue().empty())
            {
                Log(Debug::Info) << "FNV VATS capture: screenshot target=" << mFalloutVatsTargetName
                                 << " bodyPart=" << mFalloutVatsBodyPartName
                                 << " hitChance=" << mFalloutVatsHitChance
                                 << " queued=" << mFalloutVats.getQueue().size();
                screenshot();
                mFalloutVatsCaptureQueued = true;
            }

        }
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
                // The shipped Fallout profile keeps OpenMW's E-to-jump binding, while the native FNV VATS HUD uses
                // retail's [E] ACCEPT prompt. In targeting mode E is an accept action, not a world jump.
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    executeFalloutVatsQueue();
                break;
            case A_Activate:
                inputManager->resetIdleTime();
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    executeFalloutVatsQueue();
                else if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Inactive && !VR::getVR())
                    activate();
                break;
            case A_Use:
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    queueFalloutVatsAttack();
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
            case A_Journal:
            case A_QuickKeysMenu:
                // Handled in Lua
                break;
        }
    }

    bool ActionManager::isFalloutContent() const
    {
        const auto world = MWBase::Environment::get().getWorld();
        for (const std::string& file : world->getContentFiles())
        {
            std::string lower = file;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
            if (lower.find("falloutnv.esm") != std::string::npos)
                return true;
        }
        return false;
    }

    void ActionManager::toggleFalloutVats()
    {
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Inactive)
        {
            if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Executing)
            {
                finishFalloutVatsExecution(true);
                return;
            }
            mFalloutVats.cancel();
            restoreFalloutVatsView();
            updateFalloutVatsHud();
            return;
        }

        const auto world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr player = world->getPlayerPtr();
        const auto validTarget = [&](const MWWorld::Ptr& candidate) {
            return !candidate.isEmpty() && candidate != player && candidate.getClass().isActor()
                && !candidate.getClass().getCreatureStats(candidate).isDead();
        };
        std::vector<MWWorld::Ptr> nearby;
        MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
            player.getRefData().getPosition().asVec3(), 3000.f, nearby);
        std::ranges::sort(nearby, [&](const MWWorld::Ptr& left, const MWWorld::Ptr& right) {
            const osg::Vec3f origin = player.getRefData().getPosition().asVec3();
            return (left.getRefData().getPosition().asVec3() - origin).length2()
                < (right.getRefData().getPosition().asVec3() - origin).length2();
        });

        mFalloutVatsTargets.clear();
        MWWorld::Ptr target = world->getFacedObject();
        if (validTarget(target))
            mFalloutVatsTargets.push_back(target);
        for (const MWWorld::Ptr& candidate : nearby)
        {
            if (validTarget(candidate) && std::ranges::find(mFalloutVatsTargets, candidate) == mFalloutVatsTargets.end())
                mFalloutVatsTargets.push_back(candidate);
        }
        if (mFalloutVatsTargets.empty())
        {
            Log(Debug::Warning) << "FNV VATS: target acquisition failed";
            MWBase::Environment::get().getWindowManager()->messageBox("No V.A.T.S. target");
            return;
        }
        target = mFalloutVatsTargets.front();

        const auto ap = world->getFalloutPlayerRuntimeState().getCurrentActorValue(
            MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue);
        if (!ap || !mFalloutVats.enter(ap->mValue))
        {
            Log(Debug::Warning) << "FNV VATS: action points unavailable";
            MWBase::Environment::get().getWindowManager()->messageBox("V.A.T.S. action points unavailable");
            return;
        }
        world->getFalloutPlayerRuntimeState().setVatsActive(true);

        MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
        const MWWorld::ContainerStoreIterator weapon = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        MWMechanics::FalloutVatsWeaponFailure weaponFailure;
        if (weapon == inventory.end() || weapon->getType() != ESM4::Weapon::sRecordId
            || !(mFalloutVatsWeapon = MWMechanics::buildFalloutVatsWeaponContract(
                     *weapon->get<ESM4::Weapon>()->mBase, weaponFailure)))
        {
            mFalloutVats.cancel();
            Log(Debug::Warning) << "FNV VATS: equipped weapon contract unavailable";
            MWBase::Environment::get().getWindowManager()->messageBox("Equipped weapon has no authored V.A.T.S. AP contract");
            return;
        }

        MWRender::Camera* camera = world->getRenderingManager()->getCamera();
        mFalloutVatsPreviousCameraMode = static_cast<int>(camera->getMode());
        mFalloutVatsPreviousCameraDistance = camera->getCameraDistance();
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
            MWBase::Environment::get().getWindowManager()->messageBox("No target has authored V.A.T.S. limbs");
        }
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
            Log(Debug::Warning) << "FNV VATS: body data unavailable target=" << target.getClass().getName(target);
            return false;
        }

        std::vector<MWMechanics::FalloutVatsBodyPartContract> bodyParts;
        std::size_t selectedBodyPartIndex = 0;
        for (std::size_t index = 0; index < targetBodyData->mBodyParts.size() && index <= 14; ++index)
        {
            MWMechanics::FalloutVatsBodyPartFailure bodyFailure;
            auto candidate = MWMechanics::buildFalloutVatsBodyPartContract(
                targetBodyData->mBodyParts[index], static_cast<std::uint8_t>(index), bodyFailure);
            if (!candidate)
                continue;
            if (candidate->mName == "Torso")
                selectedBodyPartIndex = bodyParts.size();
            bodyParts.push_back(*candidate);
        }
        if (bodyParts.empty())
        {
            Log(Debug::Warning) << "FNV VATS: authored limbs incomplete target=" << target.getClass().getName(target);
            return false;
        }

        if (!mFalloutVatsTarget.isEmpty())
        {
            if (MWRender::Animation* animation = world->getAnimation(mFalloutVatsTarget))
                animation->setFalloutVatsWireframe({}, false);
        }
        mFalloutVatsTarget = target;
        mFalloutVatsTargetName = std::string(target.getClass().getName(target));
        mFalloutVatsBodyParts = std::move(bodyParts);
        mFalloutVatsBodyPartIndex = selectedBodyPartIndex;
        if (!selectFalloutVatsBodyPart(selectedBodyPartIndex))
            return false;
        updateFalloutVatsCamera();
        return true;
    }

    bool ActionManager::selectFalloutVatsBodyPart(std::size_t index)
    {
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Targeting
            || mFalloutVatsTarget.isEmpty() || !mFalloutVatsWeapon || index >= mFalloutVatsBodyParts.size())
            return false;

        const MWMechanics::FalloutVatsBodyPartContract& bodyPart = mFalloutVatsBodyParts[index];
        const unsigned int hitChance = bodyPart.mAbsoluteHitChance
            ? bodyPart.mBaseHitChance
            : std::min(100u, static_cast<unsigned int>(bodyPart.mBaseHitChance)
                    + static_cast<unsigned int>(mFalloutVatsWeapon->mBaseHitChance));
        if (!mFalloutVats.select(mFalloutVatsTarget.getCellRef().getRefNum(), bodyPart, hitChance))
            return false;

        mFalloutVatsBodyPartIndex = index;
        mFalloutVatsBodyPartName = std::string(bodyPart.mName);
        mFalloutVatsBodyPartTargetNode = std::string(bodyPart.mTargetNode);
        mFalloutVatsHitChance = hitChance;
        if (MWRender::Animation* animation
            = MWBase::Environment::get().getWorld()->getAnimation(mFalloutVatsTarget))
            animation->setFalloutVatsWireframe(mFalloutVatsBodyPartTargetNode, true);
        Log(Debug::Info) << "FNV VATS: selected target=" << mFalloutVatsTargetName
                         << " bodyPart=" << mFalloutVatsBodyPartName
                         << " bodyPartIndex=" << static_cast<unsigned int>(bodyPart.mIndex)
                         << " targetNode=" << mFalloutVatsBodyPartTargetNode
                         << " hitChance=" << mFalloutVatsHitChance
                         << " actionPoints=" << mFalloutVats.getActionPointsBefore();
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
        const osg::Vec3d focus = world->getActorHeadTransform(mFalloutVatsTarget).getTrans();
        const osg::Vec3d playerHead = world->getActorHeadTransform(world->getPlayerPtr()).getTrans();
        osg::Vec3d towardPlayer = playerHead - focus;
        towardPlayer.z() = 0.0;
        if (towardPlayer.normalize() == 0.0)
        {
            const float targetYaw = mFalloutVatsTarget.getRefData().getPosition().rot[2];
            towardPlayer.set(-std::cos(targetYaw), std::sin(targetYaw), 0.0);
        }
        const osg::Vec3d eye = focus + towardPlayer * 180.0;
        const osg::Vec3d lookDirection = focus - eye;
        MWRender::Camera* camera = world->getRenderingManager()->getCamera();
        camera->setMode(MWRender::Camera::Mode::Static, true);
        camera->setStaticPosition(eye);
        camera->setPitch(0.f, true);
        camera->setYaw(-static_cast<float>(std::atan2(lookDirection.x(), lookDirection.y())), true);
        camera->setRoll(0.f);
        camera->instantTransition();
        camera->updateCamera();
    }

    void ActionManager::restoreFalloutVatsView()
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        world->getFalloutPlayerRuntimeState().setVatsActive(false);
        if (!mFalloutVatsTarget.isEmpty())
        {
            if (MWRender::Animation* animation = world->getAnimation(mFalloutVatsTarget))
                animation->setFalloutVatsWireframe({}, false);
        }
        if (mFalloutVatsPreviousCameraMode >= 0)
        {
            world->getTimeManager()->setSimulationTimeScale(mFalloutVatsPreviousSimulationScale);
            MWRender::Camera* camera = world->getRenderingManager()->getCamera();
            camera->attachTo(world->getPlayerPtr());
            camera->setPreferredCameraDistance(mFalloutVatsPreviousCameraDistance);
            camera->setMode(static_cast<MWRender::Camera::Mode>(mFalloutVatsPreviousCameraMode), true);
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
        mFalloutVatsPreviousSimulationScale = 1.f;
        mFalloutVatsExecutionTimer = 0.f;
        mFalloutVatsExecutionApBefore = 0.f;
        mFalloutVatsExecutionPlannedApAfter = 0.f;
        mFalloutVatsExecutionApSpent = 0.f;
        mFalloutVatsExecutionDamage = 0.f;
        mFalloutVatsExecutionQueued = 0;
        mFalloutVatsExecutionShotsAttempted = 0;
        mFalloutVatsExecutionShotsFired = 0;
        mFalloutVatsExecutionRolledHits = 0;
    }

    void ActionManager::queueFalloutVatsAttack()
    {
        if (!mFalloutVatsWeapon)
            return;
        MWMechanics::FalloutVatsQueueFailure failure;
        if (!mFalloutVats.queueSelected(*mFalloutVatsWeapon, getFalloutVatsAvailableShots(), failure))
            MWBase::Environment::get().getWindowManager()->messageBox(
                std::string("V.A.T.S. queue failed: ") + std::string(MWMechanics::getFalloutVatsQueueFailureName(failure)));
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
            return static_cast<std::size_t>(std::max(0,
                inventory.count(ESM::RefId::formIdRefId(base.mId))));
        if (base.mData.ammoUse == 0)
            return 0;

        const auto& store = MWBase::Environment::get().getWorld()->getStore();
        std::vector<ESM::FormId> candidates;
        if (store.get<ESM4::Ammunition>().search(base.mAmmo) != nullptr)
            candidates.push_back(base.mAmmo);
        else if (const ESM4::FormIdList* list = store.get<ESM4::FormIdList>().search(base.mAmmo))
            candidates = list->mObjects;
        else
            return 0;

        std::size_t rounds = 0;
        std::vector<ESM::FormId> counted;
        for (ESM::FormId candidate : candidates)
        {
            if (candidate.isZeroOrUnset()
                || store.get<ESM4::Ammunition>().search(candidate) == nullptr
                || std::ranges::find(counted, candidate) != counted.end())
                continue;
            counted.push_back(candidate);
            rounds += static_cast<std::size_t>(std::max(0,
                inventory.count(ESM::RefId::formIdRefId(candidate))));
        }
        return rounds / base.mData.ammoUse;
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
        MWBase::Environment::get().getWorld()->getTimeManager()->setSimulationTimeScale(0.2f);
        Log(Debug::Info) << "FNV VATS execution: phase=begin queued=" << mFalloutVatsExecutionQueued
                         << " apBefore=" << mFalloutVatsExecutionApBefore
                         << " plannedApAfter=" << mFalloutVatsExecutionPlannedApAfter;
        updateFalloutVatsHud();
    }

    void ActionManager::updateFalloutVatsExecution(float dt)
    {
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Executing)
            return;
        if (std::isfinite(dt) && dt > 0.f)
            mFalloutVatsExecutionTimer += dt;
        updateFalloutVatsCamera();

        if (mFalloutVats.getExecutingAction() != nullptr)
        {
            const float delay = mFalloutVatsExecutionShotsAttempted == 0 ? 0.35f : 0.65f;
            if (mFalloutVatsExecutionTimer < delay)
                return;
            mFalloutVatsExecutionTimer = 0.f;
            if (!executeNextFalloutVatsAction())
                finishFalloutVatsExecution(true);
            return;
        }

        if (mFalloutVats.isExecutionComplete() && mFalloutVatsExecutionTimer >= 0.9f)
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
        const auto target = std::ranges::find_if(mFalloutVatsTargets, [&](const MWWorld::Ptr& candidate) {
            return !candidate.isEmpty() && candidate.getCellRef().getRefNum() == executing.mTarget;
        });
        if (target != mFalloutVatsTargets.end())
            executionTarget = *target;
        else
            executionTarget = world->searchPtr(ESM::RefId(executing.mTarget), true, false);
        if (executionTarget.isEmpty() || !executionTarget.getClass().isActor())
        {
            Log(Debug::Error) << "FNV VATS action: target=" << executing.mTarget
                              << " bodyPart=" << executing.mBodyPartName
                              << " result=target-unavailable fired=0";
            return false;
        }

        if (executionTarget != mFalloutVatsTarget)
        {
            if (!mFalloutVatsTarget.isEmpty())
            {
                if (MWRender::Animation* animation = world->getAnimation(mFalloutVatsTarget))
                    animation->setFalloutVatsWireframe({}, false);
            }
            mFalloutVatsTarget = executionTarget;
            mFalloutVatsTargetName = std::string(executionTarget.getClass().getName(executionTarget));
        }
        mFalloutVatsBodyPartName = executing.mBodyPartName;
        mFalloutVatsBodyPartTargetNode = executing.mTargetNode;
        mFalloutVatsHitChance = executing.mDisplayedHitChance;
        if (MWRender::Animation* animation = world->getAnimation(executionTarget))
            animation->setFalloutVatsWireframe(executing.mTargetNode, true);
        updateFalloutVatsCamera();
        updateFalloutVatsHud();

        const float healthBefore
            = executionTarget.getClass().getCreatureStats(executionTarget).getHealth().getCurrent();
        osg::Vec3f targetPoint = world->getActorHeadTransform(executionTarget).getTrans();
        if (MWRender::Animation* animation = world->getAnimation(executionTarget))
        {
            if (const osg::Node* targetNode = animation->getNode(executing.mTargetNode))
            {
                const osg::NodePathList paths = targetNode->getParentalNodePaths();
                if (!paths.empty())
                    targetPoint = osg::computeLocalToWorld(paths.front()).getTrans();
            }
        }
        const float hitRoll = Misc::Rng::rollProbability(world->getPrng());
        const bool rolledHit
            = MWMechanics::doesFalloutVatsAttackHit(executing.mDisplayedHitChance, hitRoll);
        const bool fired = MWBase::Environment::get().getMechanicsManager()->executeFalloutVatsRangedHit(
            world->getPlayerPtr(), executionTarget, targetPoint, executing, rolledHit);
        const float healthAfter
            = executionTarget.getClass().getCreatureStats(executionTarget).getHealth().getCurrent();
        mFalloutVatsExecutionDamage += std::max(0.f, healthBefore - healthAfter);
        Log(Debug::Info) << "FNV VATS action: target=" << executionTarget.getClass().getName(executionTarget)
                         << " bodyPart=" << executing.mBodyPartName
                         << " targetNode=" << executing.mTargetNode
                         << " displayedHitChance=" << static_cast<unsigned int>(executing.mDisplayedHitChance)
                         << " healthDamageMultiplier=" << executing.mHealthDamageMultiplier
                         << " limbDamageMultiplier=" << executing.mLimbDamageMultiplier
                         << " healthPercent=" << static_cast<unsigned int>(executing.mHealthPercent)
                         << " actorValue=" << static_cast<int>(executing.mActorValue)
                         << " roll=" << hitRoll << " rolledHit=" << rolledHit << " fired=" << fired
                         << " healthBefore=" << healthBefore << " healthAfter=" << healthAfter
                         << " damage=" << (healthBefore - healthAfter);
        if (!fired)
            return false;

        ++mFalloutVatsExecutionShotsFired;
        if (rolledHit)
            ++mFalloutVatsExecutionRolledHits;
        mFalloutVatsExecutionApSpent += executing.mActionPointCost;
        const float apAfter = std::max(0.f, mFalloutVatsExecutionApBefore - mFalloutVatsExecutionApSpent);
        world->getFalloutPlayerRuntimeState().setCurrentActorValue(
            MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue, apAfter);
        return mFalloutVats.advanceExecution();
    }

    void ActionManager::finishFalloutVatsExecution(bool interrupted)
    {
        const float apAfter
            = std::max(0.f, mFalloutVatsExecutionApBefore - mFalloutVatsExecutionApSpent);
        MWBase::Environment::get().getWorld()->getFalloutPlayerRuntimeState().setCurrentActorValue(
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
        restoreFalloutVatsView();
        updateFalloutVatsHud();
    }

    void ActionManager::updateFalloutVatsHud()
    {
        MWGui::HUD* hud = MWBase::Environment::get().getWindowManager()->getHud();
        if (hud == nullptr)
            return;
        const bool visible = mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Inactive;
        hud->setFalloutVatsVisible(visible, mFalloutVatsTargetName, mFalloutVatsBodyPartName,
            mFalloutVatsHitChance, mFalloutVats.getActionPointsBefore(), mFalloutVats.getActionPointsAfter(),
            mFalloutVats.getQueue().size(), mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Executing);
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
//## VR_PATCH BEGIN
// Vr opens a different menu with more options, normally accessed using a keybind
            if (VR::getVR())
                MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_VrMetaMenu);
            else
                MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_MainMenu);
//## VR_PATCH END
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
        if (!MWBase::Environment::get().getInputManager()->getControlSwitch("playercontrols"))
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
        else if (MWBase::Environment::get().getInputManager()->getControlSwitch("playercontrols"))
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
