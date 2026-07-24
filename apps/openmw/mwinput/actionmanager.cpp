#include "actionmanager.hpp"

#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>

#include <SDL_keyboard.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>
#include <ranges>
#include <vector>

#include <osg/Camera>
#include <osg/Matrix>
#include <osg/Viewport>
#include <osgViewer/Viewer>

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
#include <components/sceneutil/positionattitudetransform.hpp>

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "../mwrender/animation.hpp"
#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"

#include "../mwphysics/raycasting.hpp"

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
        if (const char* proof = std::getenv("OPENMW_FNV_VATS_PROOF"))
            mFalloutVatsProofEnabled = std::string_view(proof) == "1";
        if (const char* target = std::getenv("OPENMW_FNV_VATS_PROOF_TARGET"))
            mFalloutVatsProofTargetName = target;
        if (const char* weapon = std::getenv("OPENMW_FNV_VATS_PROOF_WEAPON"))
        {
            try
            {
                mFalloutVatsProofWeaponFormId = static_cast<std::uint32_t>(std::stoul(weapon, nullptr, 0));
            }
            catch (const std::exception&)
            {
                Log(Debug::Warning) << "FNV VATS proof: invalid weapon form=" << weapon
                                    << " using=0x0000434f";
            }
        }
        if (const char* step = std::getenv("OPENMW_FNV_VATS_PROOF_CAPTURE_STEP"))
        {
            try
            {
                mFalloutVatsProofCaptureStep = std::max(1u, static_cast<unsigned int>(std::stoul(step)));
            }
            catch (const std::exception&)
            {
                Log(Debug::Warning) << "FNV VATS proof: invalid capture step=" << step << " using=3";
            }
        }
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

        updateFalloutVatsProof();
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
            case A_ToggleSpell:
                // Fallout uses the existing retail R binding for reload. TES3 retains Ready Magic.
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
        MWWorld::Ptr target = world->getFacedObject();
        if (validTarget(target))
            mFalloutVatsTargets.push_back(target);
        for (const MWWorld::Ptr& candidate : nearby)
        {
            if (screenTargetScore(candidate)
                && std::ranges::find(mFalloutVatsTargets, candidate) == mFalloutVatsTargets.end())
                mFalloutVatsTargets.push_back(candidate);
        }
        if (mFalloutVatsProofEnabled && !mFalloutVatsProofTargetName.empty())
        {
            const auto preferred = std::ranges::find_if(mFalloutVatsTargets, [&](const MWWorld::Ptr& candidate) {
                return Misc::StringUtils::ciEqual(candidate.getClass().getName(candidate), mFalloutVatsProofTargetName);
            });
            if (preferred != mFalloutVatsTargets.end())
                std::rotate(mFalloutVatsTargets.begin(), preferred, std::next(preferred));
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

        // V.A.T.S. may be entered with a holstered weapon. The execution camera must
        // never show an empty hand producing a muzzle flash, so begin the ordinary
        // equipped-weapon transition before targeting freezes simulation.
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
        MWRender::Animation* animation = world->getAnimation(target);
        if (animation == nullptr)
        {
            Log(Debug::Warning) << "FNV VATS: rendered actor unavailable target="
                                << target.getClass().getName(target);
            return false;
        }
        for (std::size_t index = 0; index < targetBodyData->mBodyParts.size() && index <= 14; ++index)
        {
            MWMechanics::FalloutVatsBodyPartFailure bodyFailure;
            auto candidate = MWMechanics::buildFalloutVatsBodyPartContract(
                targetBodyData->mBodyParts[index], static_cast<std::uint8_t>(index), bodyFailure);
            if (!candidate)
                continue;
            if (animation->getNode(candidate->mTargetNode) == nullptr)
            {
                Log(Debug::Warning) << "FNV VATS: authored target node unavailable target="
                                    << target.getClass().getName(target)
                                    << " bodyPart=" << candidate->mName
                                    << " targetNode=" << candidate->mTargetNode;
                continue;
            }
            if (candidate->mName == "Torso")
                selectedBodyPartIndex = bodyParts.size();
            bodyParts.push_back(*candidate);
        }
        if (bodyParts.empty())
        {
            Log(Debug::Warning) << "FNV VATS: authored limbs incomplete target=" << target.getClass().getName(target);
            return false;
        }

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
        if (!selectFalloutVatsBodyPart(selectedBodyPartIndex))
            return false;
        return true;
    }

    bool ActionManager::selectFalloutVatsBodyPart(std::size_t index)
    {
        if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Targeting
            || mFalloutVatsTarget.isEmpty() || !mFalloutVatsWeapon || index >= mFalloutVatsBodyParts.size())
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
            // Retail frames a standing target around the upper torso, leaving enough vertical room for every limb
            // selector. Actor head and base transforms are already world-space, so no scene-bound transform belongs
            // here.
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
                    // Use a three-quarter shooter cut. A nearly straight rear view hid the rifle and both hands
                    // behind the actor's torso even while the authored attack controllers were advancing.
                    mFalloutVatsExecutionCameraFocus = playerHead + shotAxis * 15.f
                        - osg::Vec3f(0.f, 0.f, 10.f);
                    osg::Vec3f candidate = playerHead - shotAxis * 55.f + side * 70.f
                        + osg::Vec3f(0.f, 0.f, 15.f);
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
                    Log(Debug::Info) << "FNV VATS camera: execution phase=shooter shotDistance=" << shotDistance;
                }
                else
                {
                    const MWMechanics::FalloutVatsCameraPose targetPose
                        = MWMechanics::buildFalloutVatsFrontalCameraPose(
                            focus, radius, mFalloutVatsTargetFramingForward);
                    mFalloutVatsExecutionCameraEye = targetPose.mEye;
                    mFalloutVatsExecutionCameraFocus = targetPose.mFocus;
                    Log(Debug::Info) << "FNV VATS camera: execution phase=impact";
                }
                mFalloutVatsExecutionCameraInitialized = true;
                Log(Debug::Info) << "FNV VATS camera: execution eye=" << mFalloutVatsExecutionCameraEye
                                 << " focus=" << mFalloutVatsExecutionCameraFocus;
            }
            pose.mEye = mFalloutVatsExecutionCameraEye;
            pose.mFocus = mFalloutVatsExecutionCameraFocus;
        }
        else
            pose = MWMechanics::buildFalloutVatsFrontalCameraPose(
                focus, radius, mFalloutVatsTargetFramingForward);
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
        camera->updateCamera();
    }

    void ActionManager::updateFalloutVatsHighlight()
    {
        if (mFalloutVatsTarget.isEmpty())
            return;
        MWRender::Animation* animation = MWBase::Environment::get().getWorld()->getAnimation(mFalloutVatsTarget);
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

        const ESM::RefId weaponId = ESM::RefId::formIdRefId(base.mId);
        if (const std::optional<int> loaded = inventory.getFalloutLoadedAmmo(weaponId))
            return static_cast<std::size_t>(std::max(0, *loaded)) / base.mData.ammoUse;

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
            Log(Debug::Info) << "FNV VATS execution: shooter-facing previousYaw="
                             << mFalloutVatsPreviousPlayerYaw << " targetYaw=" << targetYaw;
        }
        mFalloutVatsExecutionCameraPhase = 0;
        mFalloutVatsExecutionCameraInitialized = false;
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
        if (mFalloutVats.getExecutingAction() != nullptr)
        {
            if (!mFalloutVatsExecutionVisualPrepared)
            {
                if (mFalloutVatsExecutionShotsAttempted > 0 && mFalloutVatsExecutionTimer < 0.35f)
                {
                    // Hold the shooter cut through discharge long enough for the authored muzzle flash to render,
                    // then show the target impact before preparing the next queued action.
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

            // Give the newly prepared authored attack pose at least one visible beat
            // before consuming an immediate hitscan release. Projectile families still
            // wait for their authored hit/release text key below.
            if (mFalloutVatsExecutionTimer < 0.1f)
                return;

            // The weapon animation's authored delivery text key owns deferred discharge.
            // Immediate hitscan releases still pass through the visible-pose gate above.
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
                finishFalloutVatsExecution(true);
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
        float observedDamage = 0.f;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        for (const auto& [targetId, healthBefore] : mFalloutVatsExecutionTargetHealthBefore)
        {
            MWWorld::Ptr target;
            const auto activeTarget = std::ranges::find_if(mFalloutVatsTargets, [&](const MWWorld::Ptr& candidate) {
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
        if (mFalloutVatsProofEnabled)
            mFalloutVatsProofShotsFired = mFalloutVatsExecutionShotsFired;
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
        if (camera == nullptr || viewport == nullptr || viewport->width() <= 0.0 || viewport->height() <= 0.0)
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
                = world->getActorNodeTransform(mFalloutVatsTarget, mFalloutVatsBodyParts[index].mTargetNode);
            if (!transform)
                continue;
            const osg::Vec3d window = osg::Vec3d(transform->getTrans()) * camera->getViewMatrix()
                * camera->getProjectionMatrix() * viewport->computeWindowMatrix();
            if (!std::isfinite(window.x()) || !std::isfinite(window.y()) || window.z() < 0.0 || window.z() > 1.0)
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

        // Retail V.A.T.S. selects the anatomical target under the cursor. Keep a modest
        // acquisition radius so nearby limbs remain individually selectable.
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
        const bool visible = mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Inactive;
        std::vector<MWGui::FalloutVatsBodyPartDisplay> bodyPartDisplays;
        if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting
            && !mFalloutVatsTarget.isEmpty() && mFalloutVatsWeapon && mViewer != nullptr)
        {
            const osg::Camera* renderCamera = mViewer->getCamera();
            const osg::Viewport* viewport = renderCamera != nullptr ? renderCamera->getViewport() : nullptr;
            if (renderCamera != nullptr && viewport != nullptr
                && viewport->width() > 0.0 && viewport->height() > 0.0)
            {
                MWBase::World* world = MWBase::Environment::get().getWorld();
                bodyPartDisplays.reserve(mFalloutVatsBodyParts.size());
                for (std::size_t index = 0; index < mFalloutVatsBodyParts.size(); ++index)
                {
                    const MWMechanics::FalloutVatsBodyPartContract& bodyPart = mFalloutVatsBodyParts[index];
                    const std::optional<osg::Matrixf> transform
                        = world->getActorNodeTransform(mFalloutVatsTarget, bodyPart.mTargetNode);
                    if (!transform)
                        continue;
                    const osg::Vec3d window = osg::Vec3d(transform->getTrans()) * renderCamera->getViewMatrix()
                        * renderCamera->getProjectionMatrix() * viewport->computeWindowMatrix();
                    const float viewportX
                        = static_cast<float>((window.x() - viewport->x()) / viewport->width());
                    const float viewportY
                        = 1.f - static_cast<float>((window.y() - viewport->y()) / viewport->height());
                    if (!std::isfinite(viewportX) || !std::isfinite(viewportY))
                        continue;
                    bodyPartDisplays.push_back({ bodyPart.mName,
                        MWMechanics::getFalloutVatsDisplayedHitChance(bodyPart, *mFalloutVatsWeapon),
                        viewportX, viewportY, index == mFalloutVatsBodyPartIndex });
                }
            }
        }
        hud->setFalloutVatsVisible(visible, mFalloutVatsTargetName, bodyPartDisplays,
            mFalloutVats.getActionPointsBefore(), mFalloutVats.getActionPointsAfter(),
            mFalloutVats.getQueue().size(), visible ? getFalloutVatsAvailableShots() : 0,
            mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Executing);
    }

    void ActionManager::captureFalloutVatsProofFrame()
    {
        if (mScreenCaptureHandler == nullptr || mViewer == nullptr)
            return;
        mScreenCaptureHandler->setFramesToCapture(1);
        mScreenCaptureHandler->captureNextFrame(*mViewer);
        ++mFalloutVatsProofCaptures;
    }

    void ActionManager::updateFalloutVatsProof()
    {
        if (!mFalloutVatsProofEnabled || mFalloutVatsProofFinished)
            return;

        ++mFalloutVatsProofFrame;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world->getPlayerPtr();
        const auto quitWithFailure = [&](std::string_view reason) {
            Log(Debug::Error) << "FNV VATS proof: result=fail stage=" << mFalloutVatsProofStage
                              << " reason=" << reason << " captures=" << mFalloutVatsProofCaptures;
            mFalloutVatsProofFinished = true;
            MWBase::Environment::get().getStateManager()->requestQuit();
        };

        switch (mFalloutVatsProofStage)
        {
            case 0:
            {
                // Let the native save finish instantiating actors, animations, HUD, and the first rendered pose.
                if (mFalloutVatsProofFrame < (mFalloutVatsProofWeaponSelected ? 90u : 180u))
                    return;
                if (!mFalloutVatsProofWeaponSelected)
                {
                    MWWorld::InventoryStore& proofInventory = player.getClass().getInventoryStore(player);
                    const auto ap = world->getFalloutPlayerRuntimeState().getCurrentActorValue(
                        MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue);
                    MWWorld::ContainerStoreIterator proofWeapon = proofInventory.end();
                    float proofLethalCapacity = -1.f;
                    if (ap)
                    {
                        for (MWWorld::ContainerStoreIterator candidate = proofInventory.begin();
                             candidate != proofInventory.end(); ++candidate)
                        {
                            if (candidate->getCellRef().getCount() <= 0
                                || candidate->getType() != ESM4::Weapon::sRecordId)
                                continue;
                            const ESM4::Weapon& weapon = *candidate->get<ESM4::Weapon>()->mBase;
                            // ESM4 records are remapped to OpenMW's internal content-file index while loading.
                            // Match the stable authored record index and exact name, not the rewritten high byte.
                            if (weapon.mId.mIndex != (mFalloutVatsProofWeaponFormId & 0x00ffffff)
                                || !Misc::StringUtils::ciEqual(weapon.mFullName, "10mm Pistol"))
                                continue;
                            MWMechanics::FalloutVatsWeaponFailure failure;
                            const std::optional<MWMechanics::FalloutVatsWeaponContract> contract
                                = MWMechanics::buildFalloutVatsWeaponContract(weapon, failure);
                            if (!contract || contract->mActionPointCost <= 0.f || weapon.mData.ammoUse == 0)
                                continue;
                            std::vector<ESM::FormId> ammoForms;
                            const auto& store = world->getStore();
                            if (store.get<ESM4::Ammunition>().search(weapon.mAmmo) != nullptr)
                                ammoForms.push_back(weapon.mAmmo);
                            else if (const ESM4::FormIdList* list
                                = store.get<ESM4::FormIdList>().search(weapon.mAmmo))
                                ammoForms = list->mObjects;
                            std::size_t rounds = 0;
                            for (const ESM::FormId ammo : ammoForms)
                            {
                                if (!ammo.isZeroOrUnset()
                                    && store.get<ESM4::Ammunition>().search(ammo) != nullptr)
                                    rounds += static_cast<std::size_t>(std::max(
                                        0, proofInventory.count(ESM::RefId::formIdRefId(ammo))));
                            }
                            const std::size_t actions = std::min(rounds / weapon.mData.ammoUse,
                                static_cast<std::size_t>(std::floor(ap->mValue / contract->mActionPointCost)));
                            const float capacity = static_cast<float>(weapon.mData.damage) * actions;
                            Log(Debug::Info) << "FNV VATS proof weapon candidate: form=" << weapon.mId
                                             << " name=" << weapon.mFullName
                                             << " damage=" << weapon.mData.damage
                                             << " apCost=" << contract->mActionPointCost << " actions=" << actions
                                             << " lethalCapacity=" << capacity;
                            if (actions >= 4)
                            {
                                proofLethalCapacity = capacity;
                                proofWeapon = candidate;
                            }
                            break;
                        }
                    }
                    if (proofWeapon == proofInventory.end())
                    {
                        quitWithFailure("required-10mm-cannot-queue-four-shots");
                        return;
                    }
                    proofInventory.equip(MWWorld::InventoryStore::Slot_CarriedRight, proofWeapon);
                    MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);
                    Log(Debug::Info) << "FNV VATS proof weapon selected: form="
                                     << proofWeapon->get<ESM4::Weapon>()->mBase->mId
                                     << " name=" << proofWeapon->get<ESM4::Weapon>()->mBase->mFullName
                                     << " lethalCapacity=" << proofLethalCapacity;
                    mFalloutVatsProofWeaponSelected = true;
                    mFalloutVatsProofFrame = 0;
                    return;
                }
                MWRender::Camera* camera = world->getRenderingManager()->getCamera();
                mFalloutVatsProofCameraModeBefore = static_cast<int>(camera->getMode());
                mFalloutVatsProofCameraPitchBefore = camera->getPitch();
                mFalloutVatsProofCameraYawBefore = camera->getYaw();
                mFalloutVatsProofCameraRollBefore = camera->getRoll();
                mFalloutVatsProofPlayerYawBefore = player.getRefData().getPosition().rot[2];
                toggleFalloutVats();
                if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Targeting)
                {
                    quitWithFailure("enter-vats");
                    return;
                }
                if (!mFalloutVatsProofTargetName.empty()
                    && !Misc::StringUtils::ciEqual(mFalloutVatsTargetName, mFalloutVatsProofTargetName))
                {
                    quitWithFailure("named-target-not-selected");
                    return;
                }
                mFalloutVatsProofTarget = mFalloutVatsTarget;
                mFalloutVatsProofHealthBefore
                    = mFalloutVatsProofTarget.getClass().getCreatureStats(mFalloutVatsProofTarget).getHealth().getCurrent();
                Log(Debug::Info) << "FNV VATS proof: stage=targeting target=" << mFalloutVatsTargetName
                                 << " healthBefore=" << mFalloutVatsProofHealthBefore;
                mFalloutVatsProofStage = 1;
                mFalloutVatsProofFrame = 0;
                captureFalloutVatsProofFrame();
                return;
            }
            case 1:
            {
                if (mFalloutVatsProofFrame % mFalloutVatsProofCaptureStep == 0)
                    captureFalloutVatsProofFrame();
                if (mFalloutVatsProofFrame == 20 && mFalloutVatsBodyParts.size() > 1)
                {
                    cycleFalloutVatsBodyPart(1);
                    Log(Debug::Info) << "FNV VATS proof: stage=limb-selected bodyPart=" << mFalloutVatsBodyPartName;
                }
                if (mFalloutVatsProofFrame == 26 && mFalloutVatsBodyParts.size() > 1)
                {
                    cycleFalloutVatsBodyPart(-1);
                    Log(Debug::Info) << "FNV VATS proof: stage=torso-restored bodyPart=" << mFalloutVatsBodyPartName;
                }
                if (mFalloutVatsProofFrame == 30)
                {
                    queueFalloutVatsAttack();
                    if (mFalloutVats.getQueue().empty())
                    {
                        quitWithFailure("queue-attack");
                        return;
                    }
                    Log(Debug::Info) << "FNV VATS proof: stage=queued bodyPart=" << mFalloutVatsBodyPartName;
                }
                if (mFalloutVatsProofFrame == 36)
                {
                    queueFalloutVatsAttack();
                    Log(Debug::Info) << "FNV VATS proof: stage=queued-second queue="
                                     << mFalloutVats.getQueue().size();
                }
                if (mFalloutVatsProofFrame == 42)
                {
                    queueFalloutVatsAttack();
                    if (mFalloutVats.getQueue().size() != 3)
                    {
                        quitWithFailure("queue-three-10mm-shots");
                        return;
                    }
                    Log(Debug::Info) << "FNV VATS proof: stage=queued-third queue="
                                     << mFalloutVats.getQueue().size();
                }
                if (mFalloutVatsProofFrame == 48)
                {
                    queueFalloutVatsAttack();
                    if (mFalloutVats.getQueue().size() != 4)
                    {
                        quitWithFailure("queue-four-10mm-shots");
                        return;
                    }
                    Log(Debug::Info) << "FNV VATS proof: stage=queued-fourth queue="
                                     << mFalloutVats.getQueue().size();
                }
                if (mFalloutVatsProofFrame < 90)
                    return;
                executeFalloutVatsQueue();
                if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Executing)
                {
                    quitWithFailure("begin-execution");
                    return;
                }
                mFalloutVatsProofStage = 2;
                mFalloutVatsProofFrame = 0;
                return;
            }
            case 2:
            {
                if (mFalloutVatsProofFrame % mFalloutVatsProofCaptureStep == 0)
                    captureFalloutVatsProofFrame();
                if (mFalloutVats.getPhase() != MWMechanics::FalloutVatsPhase::Inactive)
                {
                    // The proof renderer is intentionally uncapped and can advance hundreds of render frames
                    // during one second of wall time. VATS, meanwhile, runs simulation at 0.2x and must retain
                    // its post-shot impact hold. Keep this only as a runaway guard; execution completion itself
                    // is governed by simulation time and pending projectile state above.
                    if (mFalloutVatsProofFrame > 12000)
                        quitWithFailure("execution-timeout");
                    return;
                }
                mFalloutVatsProofStage = 3;
                mFalloutVatsProofFrame = 0;
                Log(Debug::Info) << "FNV VATS proof: stage=post-execution";
                return;
            }
            case 3:
            {
                if (mFalloutVatsProofFrame % mFalloutVatsProofCaptureStep == 0)
                    captureFalloutVatsProofFrame();
                if (mFalloutVatsProofFrame < 180)
                    return;
                if (mFalloutVatsProofTarget.isEmpty())
                {
                    quitWithFailure("target-lost");
                    return;
                }
                const float healthAfter = mFalloutVatsProofTarget.getClass()
                    .getCreatureStats(mFalloutVatsProofTarget).getHealth().getCurrent();
                const bool damaged = std::isfinite(healthAfter) && healthAfter < mFalloutVatsProofHealthBefore;
                const bool killed = std::isfinite(healthAfter) && healthAfter <= 0.f
                    && mFalloutVatsProofTarget.getClass().getCreatureStats(mFalloutVatsProofTarget).isDead();
                const bool aggro = mFalloutVatsProofTarget.getClass().getCreatureStats(mFalloutVatsProofTarget)
                    .getAiSequence().isInCombat(player);
                std::vector<MWWorld::Ptr> nearbyActors;
                MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                    player.getRefData().getPosition().asVec3(), 4096.f, nearbyActors);
                std::size_t hostileWitnesses = 0;
                for (const MWWorld::Ptr& actor : nearbyActors)
                {
                    if (actor.isEmpty() || actor == player || actor == mFalloutVatsProofTarget
                        || !actor.getClass().isActor() || !actor.getRefData().isEnabled()
                        || actor.getClass().getCreatureStats(actor).isDead())
                        continue;
                    if (actor.getClass().getCreatureStats(actor).getAiSequence().isInCombat(player))
                        ++hostileWitnesses;
                }
                const int bounty = player.getClass().getNpcStats(player).getBounty();
                const bool witnessResponse = hostileWitnesses > 0 || bounty > 0;
                const bool requireWitnessResponse
                    = std::getenv("OPENMW_FNV_VATS_PROOF_REQUIRE_WITNESSES") != nullptr;
                const bool requireKill = std::getenv("OPENMW_FNV_VATS_PROOF_REQUIRE_KILL") != nullptr;
                const MWRender::Camera* camera = world->getRenderingManager()->getCamera();
                const float pitchDelta = std::abs(camera->getPitch() - mFalloutVatsProofCameraPitchBefore);
                const float yawDelta = std::abs(camera->getYaw() - mFalloutVatsProofCameraYawBefore);
                const float rollDelta = std::abs(camera->getRoll() - mFalloutVatsProofCameraRollBefore);
                const bool cameraRestored = static_cast<int>(camera->getMode()) == mFalloutVatsProofCameraModeBefore
                    && pitchDelta < 0.001f && yawDelta < 0.001f && rollDelta < 0.001f;
                const bool fired = mFalloutVatsProofShotsFired > 0;
                const float playerYawDelta = std::abs(std::remainder(
                    player.getRefData().getPosition().rot[2] - mFalloutVatsProofPlayerYawBefore,
                    static_cast<float>(2.0 * osg::PI)));
                const bool playerYawRestored = playerYawDelta < 0.001f;
                const bool passed = fired && damaged && (!requireKill || killed) && aggro
                    && cameraRestored && playerYawRestored
                    && (!requireWitnessResponse || witnessResponse);
                Log(passed ? Debug::Info : Debug::Error)
                    << "FNV VATS proof: result=" << (passed ? "pass" : "fail")
                     << " target=" << mFalloutVatsProofTarget.getClass().getName(mFalloutVatsProofTarget)
                     << " fired=" << fired << " damaged=" << damaged << " killed=" << killed
                     << " aggro=" << aggro
                     << " hostileWitnesses=" << hostileWitnesses << " bounty=" << bounty
                     << " witnessResponse=" << witnessResponse << " requireKill=" << requireKill
                     << " healthBefore=" << mFalloutVatsProofHealthBefore << " healthAfter=" << healthAfter
                     << " cameraRestored=" << cameraRestored << " pitchDelta=" << pitchDelta
                     << " yawDelta=" << yawDelta << " rollDelta=" << rollDelta
                     << " playerYawRestored=" << playerYawRestored << " playerYawDelta=" << playerYawDelta
                     << " captures=" << mFalloutVatsProofCaptures;
                mFalloutVatsProofFinished = true;
                MWBase::Environment::get().getStateManager()->requestQuit();
                return;
            }
            default:
                quitWithFailure("invalid-stage");
                return;
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
        const bool fallout = isFalloutContent();
        if (!MWBase::Environment::get().getInputManager()->getControlSwitch("playercontrols")
            || !MWBase::Environment::get().getInputManager()->getControlSwitch("playerfighting")
            || (!fallout && !MWBase::Environment::get().getInputManager()->getControlSwitch("playermagic")))
            return;
        if (!checkAllowedToUseItems())
            return;

        // Fallout hotkeys are inventory/weapon controls. They must not depend on Morrowind's magic switch or
        // chargen global; neither is part of the retail FNV input contract and either may be disabled or absent.
        if (!fallout
            && MWBase::Environment::get().getWorld()->getGlobalFloat(MWWorld::Globals::sCharGenState) != -1)
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
