#include "actionmanager.hpp"

#include <MyGUI_InputManager.h>

#include <SDL_keyboard.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <vector>

#include <components/settings/values.hpp>
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
#include "../mwworld/globals.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvplayerruntimestate.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/player.hpp"

#include <components/esm4/loadbptd.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadweap.hpp>

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

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

        if (std::getenv("OPENMW_FNV_VATS_CAPTURE") != nullptr && !mFalloutVatsCaptureQueued
            && isFalloutContent())
        {
            mFalloutVatsCaptureTimer += dt;
            if (mFalloutVatsCaptureTimer >= 2.f
                && mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Inactive)
                toggleFalloutVats();
            if (mFalloutVatsCaptureTimer >= 2.5f
                && mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting
                && mFalloutVats.getQueue().empty())
                queueFalloutVatsAttack();
            if (mFalloutVatsCaptureTimer >= 3.f && !mFalloutVats.getQueue().empty())
            {
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
            case A_Activate:
                inputManager->resetIdleTime();
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    executeFalloutVatsQueue();
                else if (!VR::getVR())
                    activate();
                break;
            case A_Use:
                if (mFalloutVats.getPhase() == MWMechanics::FalloutVatsPhase::Targeting)
                    queueFalloutVatsAttack();
                break;
            case A_MoveLeft:
            case A_MoveRight:
            case A_MoveForward:
            case A_MoveBackward:
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
            mFalloutVats.cancel();
            mFalloutVatsTarget = {};
            updateFalloutVatsHud();
            return;
        }

        const auto world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr player = world->getPlayerPtr();
        MWWorld::Ptr target = world->getFacedObject();
        if (target.isEmpty() || !target.getClass().isActor())
        {
            std::vector<MWWorld::Ptr> nearby;
            MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                player.getRefData().getPosition().asVec3(), 3000.f, nearby);
            float bestDistance2 = std::numeric_limits<float>::infinity();
            for (const MWWorld::Ptr& candidate : nearby)
            {
                if (candidate == player || candidate.isEmpty() || !candidate.getClass().isActor())
                    continue;
                const float distance2 = (candidate.getRefData().getPosition().asVec3()
                    - player.getRefData().getPosition().asVec3()).length2();
                if (distance2 < bestDistance2)
                {
                    bestDistance2 = distance2;
                    target = candidate;
                }
            }
        }
        if (target.isEmpty() || !target.getClass().isActor())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("No V.A.T.S. target");
            return;
        }

        const auto ap = world->getFalloutPlayerRuntimeState().getCurrentActorValue(
            MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue);
        if (!ap || !mFalloutVats.enter(ap->mValue))
        {
            MWBase::Environment::get().getWindowManager()->messageBox("V.A.T.S. action points unavailable");
            return;
        }

        MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
        const MWWorld::ContainerStoreIterator weapon = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        MWMechanics::FalloutVatsWeaponFailure weaponFailure;
        if (weapon == inventory.end() || weapon->getType() != ESM4::Weapon::sRecordId
            || !(mFalloutVatsWeapon = MWMechanics::buildFalloutVatsWeaponContract(
                     *weapon->get<ESM4::Weapon>()->mBase, weaponFailure)))
        {
            mFalloutVats.cancel();
            MWBase::Environment::get().getWindowManager()->messageBox("Equipped weapon has no authored V.A.T.S. AP contract");
            return;
        }

        const ESM4::BodyPartData* targetBodyData = nullptr;
        if (target.getType() == ESM4::Npc::sRecordId)
        {
            const ESM4::Npc* npc = target.get<ESM4::Npc>()->mBase;
            if (const ESM4::Race* race = world->getStore().get<ESM4::Race>().search(npc->mRace))
                targetBodyData = world->getStore().get<ESM4::BodyPartData>().search(race->mBodyPartData);
        }
        if (targetBodyData == nullptr)
        {
            std::string targetToken(target.getClass().getName(target));
            std::erase_if(targetToken, [](unsigned char c) { return !std::isalnum(c); });
            std::ranges::transform(targetToken, targetToken.begin(), [](unsigned char c) { return std::tolower(c); });
            if (targetToken.starts_with("young"))
                targetToken.erase(0, 5);
            for (const ESM4::BodyPartData& candidate : world->getStore().get<ESM4::BodyPartData>())
            {
                std::string editor = candidate.mEditorId;
                std::ranges::transform(editor, editor.begin(), [](unsigned char c) { return std::tolower(c); });
                if (!targetToken.empty() && editor.find(targetToken) != std::string::npos)
                {
                    targetBodyData = &candidate;
                    break;
                }
            }
        }
        if (targetBodyData == nullptr)
        {
            mFalloutVats.cancel();
            MWBase::Environment::get().getWindowManager()->messageBox("Target has no authored V.A.T.S. body data");
            return;
        }

        std::optional<MWMechanics::FalloutVatsBodyPartContract> selectedBodyPart;
        for (std::size_t index = 0; index < targetBodyData->mBodyParts.size() && index <= 14; ++index)
        {
            MWMechanics::FalloutVatsBodyPartFailure bodyFailure;
            auto candidate = MWMechanics::buildFalloutVatsBodyPartContract(
                targetBodyData->mBodyParts[index], static_cast<std::uint8_t>(index), bodyFailure);
            if (candidate && (candidate->mName == "Torso" || !selectedBodyPart))
                selectedBodyPart = candidate;
            if (selectedBodyPart && selectedBodyPart->mName == "Torso")
                break;
        }
        const ESM::FormId targetId = target.getCellRef().getRefNum();
        if (!selectedBodyPart)
        {
            mFalloutVats.cancel();
            MWBase::Environment::get().getWindowManager()->messageBox("Target V.A.T.S. limbs are incomplete");
            return;
        }
        const unsigned int hitChance = selectedBodyPart->mAbsoluteHitChance
            ? selectedBodyPart->mBaseHitChance
            : std::min(100u, static_cast<unsigned int>(selectedBodyPart->mBaseHitChance)
                    + static_cast<unsigned int>(mFalloutVatsWeapon->mBaseHitChance));
        if (!mFalloutVats.select(targetId, *selectedBodyPart, hitChance))
        {
            mFalloutVats.cancel();
            return;
        }
        mFalloutVatsTarget = target;
        mFalloutVatsTargetName = std::string(target.getClass().getName(target));
        mFalloutVatsBodyPartName = std::string(selectedBodyPart->mName);
        mFalloutVatsHitChance = hitChance;
        updateFalloutVatsHud();
    }

    void ActionManager::queueFalloutVatsAttack()
    {
        if (!mFalloutVatsWeapon)
            return;
        MWMechanics::FalloutVatsQueueFailure failure;
        if (!mFalloutVats.queueSelected(*mFalloutVatsWeapon, failure))
            MWBase::Environment::get().getWindowManager()->messageBox(
                std::string("V.A.T.S. queue failed: ") + std::string(MWMechanics::getFalloutVatsQueueFailureName(failure)));
        updateFalloutVatsHud();
    }

    void ActionManager::executeFalloutVatsQueue()
    {
        const std::optional<float> apAfter = mFalloutVats.beginExecution();
        if (!apAfter || mFalloutVatsTarget.isEmpty())
            return;
        MWBase::Environment::get().getWorld()->getFalloutPlayerRuntimeState().setCurrentActorValue(
            MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue, *apAfter);
        updateFalloutVatsHud();
        while (const MWMechanics::FalloutVatsQueuedAction* action = mFalloutVats.getExecutingAction())
        {
            MWBase::Environment::get().getMechanicsManager()->executeFalloutVatsRangedHit(
                MWBase::Environment::get().getWorld()->getPlayerPtr(), mFalloutVatsTarget,
                mFalloutVatsTarget.getRefData().getPosition().asVec3(), action->mDamageMultiplier);
            mFalloutVats.advanceExecution();
        }
        mFalloutVats.cancel();
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
