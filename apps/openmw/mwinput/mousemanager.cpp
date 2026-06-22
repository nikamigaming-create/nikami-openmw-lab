#include "mousemanager.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>

#include <osg/NodeVisitor>

#include <components/sdlutil/sdlinputwrapper.hpp>
#include <components/sdlutil/sdlmappings.hpp>
#include <components/debug/debuglog.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadmstt.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwgui/settingswindow.hpp"

#include "../mwclass/esm4npc.hpp"

#include "../mwmechanics/creaturestats.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/refdata.hpp"

#include "actions.hpp"
#include "bindingsmanager.hpp"

namespace MWInput
{
    namespace
    {
        bool hasFalloutContent()
        {
            if (std::getenv("OPENMW_FNV_TARGET_DIAG") != nullptr)
                return true;

            const MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return false;

            for (const std::string& file : world->getContentFiles())
                if (Misc::StringUtils::ciEndsWith(file, "FalloutNV.esm"))
                    return true;

            return false;
        }

        std::string formIdText(const ESM::FormId& id)
        {
            return ESM::RefId(id).toDebugString();
        }

        std::string joinFormIds(const std::vector<ESM::FormId>& ids, std::size_t maxCount = 12)
        {
            std::ostringstream stream;
            for (std::size_t i = 0; i < ids.size() && i < maxCount; ++i)
            {
                if (i != 0)
                    stream << ",";
                stream << formIdText(ids[i]);
            }
            if (ids.size() > maxCount)
                stream << ",+" << (ids.size() - maxCount) << " more";
            return stream.str();
        }

        std::string safeName(const MWWorld::Ptr& ptr)
        {
            try
            {
                return std::string(ptr.getClass().getName(ptr));
            }
            catch (const std::exception& e)
            {
                return std::string("<name error: ") + e.what() + ">";
            }
        }

        std::string safeModel(const MWWorld::Ptr& ptr)
        {
            try
            {
                return ptr.getClass().getCorrectedModel(ptr).value();
            }
            catch (const std::exception& e)
            {
                return std::string("<model error: ") + e.what() + ">";
            }
        }

        class FalloutTargetNodeDiagVisitor final : public osg::NodeVisitor
        {
        public:
            FalloutTargetNodeDiagVisitor()
                : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                ++mTotalNodes;
                const std::string& name = node.getName();
                if (!name.empty())
                {
                    ++mNamedNodes;
                    const std::string lower = Misc::StringUtils::lowerCase(name);
                    if (isInteresting(lower) && mInterestingNames.size() < 64
                        && std::find(mInterestingNames.begin(), mInterestingNames.end(), name) == mInterestingNames.end())
                    {
                        mInterestingNames.push_back(name);
                    }
                }
                traverse(node);
            }

            std::string summary() const
            {
                std::ostringstream stream;
                stream << "nodes=" << mTotalNodes << " named=" << mNamedNodes << " interesting=[";
                for (std::size_t i = 0; i < mInterestingNames.size(); ++i)
                {
                    if (i != 0)
                        stream << " | ";
                    stream << mInterestingNames[i];
                }
                stream << "]";
                return stream.str();
            }

        private:
            static bool isInteresting(const std::string& lower)
            {
                static constexpr std::string_view terms[] = {
                    "bip01", "bone", "head", "hair", "eye", "mouth", "teeth", "tongue", "face", "skin",
                    "body", "hand", "neck", "spine", "pelvis", "npc", "fnv", "fallout", "weapon",
                };

                for (std::string_view term : terms)
                    if (lower.find(term) != std::string::npos)
                        return true;
                return false;
            }

            std::size_t mTotalNodes = 0;
            std::size_t mNamedNodes = 0;
            std::vector<std::string> mInterestingNames;
        };

        void logEsm4NpcDiagnostic(const MWWorld::Ptr& ptr)
        {
            if (ptr.getType() != ESM4::Npc::sRecordId)
                return;

            const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(ptr);
            const ESM4::Npc* model = MWClass::ESM4Npc::getModelRecord(ptr);
            const ESM4::Race* race = MWClass::ESM4Npc::getRace(ptr);

            Log(Debug::Info) << "FNV/ESM4 target diag: npc traits="
                             << (traits == nullptr ? std::string("<null>") : traits->mEditorId)
                             << " traitsForm=" << (traits == nullptr ? std::string("<null>") : formIdText(traits->mId))
                             << " modelRecord=" << (model == nullptr ? std::string("<null>") : model->mEditorId)
                             << " race=" << (race == nullptr ? std::string("<null>") : race->mEditorId)
                             << " female=" << (traits != nullptr ? MWClass::ESM4Npc::isFemale(ptr) : false)
                             << " hair=" << (traits == nullptr ? std::string("<null>") : formIdText(traits->mHair))
                             << " eyes=" << (traits == nullptr ? std::string("<null>") : formIdText(traits->mEyes))
                             << " headParts=["
                             << (traits == nullptr ? std::string() : joinFormIds(traits->mHeadParts)) << "]"
                             << " tintLayers=" << (traits == nullptr ? 0 : traits->mTintLayers.size())
                             << " kf=["
                             << (model == nullptr ? std::string() : [&]() {
                                    std::ostringstream stream;
                                    for (std::size_t i = 0; i < model->mKf.size() && i < 12; ++i)
                                    {
                                        if (i != 0)
                                            stream << ",";
                                        stream << model->mKf[i];
                                    }
                                    if (model->mKf.size() > 12)
                                        stream << ",+" << (model->mKf.size() - 12) << " more";
                                    return stream.str();
                                }())
                             << "]";
        }

        void logEsm4CreatureDiagnostic(const MWWorld::Ptr& ptr)
        {
            if (ptr.getType() != ESM4::Creature::sRecordId)
                return;

            const MWWorld::LiveCellRef<ESM4::Creature>* ref = ptr.get<ESM4::Creature>();
            const ESM4::Creature* creature = ref == nullptr ? nullptr : ref->mBase;
            if (creature == nullptr)
                return;

            std::ostringstream kf;
            for (std::size_t i = 0; i < creature->mKf.size() && i < 12; ++i)
            {
                if (i != 0)
                    kf << ",";
                kf << creature->mKf[i];
            }
            if (creature->mKf.size() > 12)
                kf << ",+" << (creature->mKf.size() - 12) << " more";

            Log(Debug::Info) << "FNV/ESM4 target diag: creature editor=" << creature->mEditorId
                             << " form=" << formIdText(creature->mId) << " fullName=" << creature->mFullName
                             << " model=" << creature->mModel << " bodyParts=[" << joinFormIds(creature->mBodyParts)
                             << "] kf=[" << kf.str() << "] flags=0x" << std::hex << creature->mBaseConfig.fo3.flags
                             << std::dec << " health=" << creature->mData.health << " damage=" << creature->mData.damage;
        }

        void logEsm4MovableStaticDiagnostic(const MWWorld::Ptr& ptr)
        {
            if (ptr.getType() != ESM4::MovableStatic::sRecordId)
                return;

            const MWWorld::LiveCellRef<ESM4::MovableStatic>* ref = ptr.get<ESM4::MovableStatic>();
            const ESM4::MovableStatic* movable = ref == nullptr ? nullptr : ref->mBase;
            if (movable == nullptr)
                return;

            Log(Debug::Info) << "FNV/ESM4 target diag: movable static editor=" << movable->mEditorId
                             << " form=" << formIdText(movable->mId) << " fullName=" << movable->mFullName
                             << " model=" << movable->mModel << " data=" << static_cast<int>(movable->mData)
                             << " loopingSound=" << formIdText(movable->mLoopingSound);
        }

        void logEsm3ActorDiagnostic(const MWWorld::Ptr& ptr)
        {
            if (ptr.getType() == ESM::NPC::sRecordId)
            {
                const MWWorld::LiveCellRef<ESM::NPC>* npcRef = ptr.get<ESM::NPC>();
                if (npcRef == nullptr || npcRef->mBase == nullptr)
                    return;

                Log(Debug::Info) << "FNV/ESM4 target diag: esm3 npc id=" << npcRef->mBase->mId
                                 << " race=" << npcRef->mBase->mRace << " class=" << npcRef->mBase->mClass
                                 << " hair=" << npcRef->mBase->mHair << " head=" << npcRef->mBase->mHead
                                 << " model=" << npcRef->mBase->mModel;
            }
            else if (ptr.getType() == ESM::Creature::sRecordId)
            {
                const MWWorld::LiveCellRef<ESM::Creature>* creatureRef = ptr.get<ESM::Creature>();
                if (creatureRef == nullptr || creatureRef->mBase == nullptr)
                    return;

                Log(Debug::Info) << "FNV/ESM4 target diag: esm3 creature id=" << creatureRef->mBase->mId
                                 << " model=" << creatureRef->mBase->mModel
                                 << " creatureType=" << creatureRef->mBase->mData.mType
                                 << " level=" << creatureRef->mBase->mData.mLevel
                                 << " health=" << creatureRef->mBase->mData.mHealth;
            }
        }

        void logFalloutFocusDiagnostic()
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            MWWorld::Ptr target = world == nullptr ? MWWorld::Ptr() : world->getFocusObject();
            if (target.isEmpty())
            {
                Log(Debug::Info) << "FNV/ESM4 target diag: middle-click focus is empty";
                return;
            }

            const ESM::Position& pos = target.getRefData().getPosition();
            SceneUtil::PositionAttitudeTransform* baseNode = target.getRefData().getBaseNode();

            Log(Debug::Info) << "FNV/ESM4 target diag: BEGIN " << target.toString()
                             << " name=\"" << safeName(target) << "\" type=0x" << std::hex << target.getType()
                             << std::dec << " typeDesc=" << target.getTypeDescription()
                             << " actor=" << target.getClass().isActor() << " npc=" << target.getClass().isNpc()
                             << " model=" << safeModel(target) << " pos=(" << pos.pos[0] << "," << pos.pos[1]
                             << "," << pos.pos[2] << ") rot=(" << pos.rot[0] << "," << pos.rot[1] << ","
                             << pos.rot[2] << ") enabled=" << target.getRefData().isEnabled()
                             << " baseNode=" << (baseNode != nullptr);

            if (target.getClass().isActor())
            {
                try
                {
                    const MWMechanics::CreatureStats& stats = target.getClass().getCreatureStats(target);
                    Log(Debug::Info) << "FNV/ESM4 target diag: actor stats level=" << stats.getLevel()
                                     << " dead=" << stats.isDead() << " health="
                                     << stats.getHealth().getCurrent() << "/" << stats.getHealth().getModified()
                                     << " fatigue=" << stats.getFatigue().getCurrent() << "/"
                                     << stats.getFatigue().getModified();
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "FNV/ESM4 target diag: actor stats failed: " << e.what();
                }
            }

            logEsm4NpcDiagnostic(target);
            logEsm4CreatureDiagnostic(target);
            logEsm4MovableStaticDiagnostic(target);
            logEsm3ActorDiagnostic(target);

            if (baseNode != nullptr)
            {
                FalloutTargetNodeDiagVisitor visitor;
                baseNode->accept(visitor);
                Log(Debug::Info) << "FNV/ESM4 target diag: scene graph " << visitor.summary();
            }
            else
                Log(Debug::Warning) << "FNV/ESM4 target diag: no rendered base node for focus object";

            Log(Debug::Info) << "FNV/ESM4 target diag: END " << target.getCellRef().getRefId().toDebugString();
        }
    }

    MouseManager::MouseManager(
        BindingsManager* bindingsManager, SDLUtil::InputWrapper* inputWrapper, SDL_Window* window)
        : mBindingsManager(bindingsManager)
        , mInputWrapper(inputWrapper)
        , mGuiCursorX(0)
        , mGuiCursorY(0)
        , mMouseWheel(0)
        , mMouseLookEnabled(false)
        , mGuiCursorEnabled(true)
        , mLastWarpX(-1)
        , mLastWarpY(-1)
        , mMouseMoveX(0)
        , mMouseMoveY(0)
    {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);

        float uiScale = MWBase::Environment::get().getWindowManager()->getScalingFactor();
        mGuiCursorX = w / (2.f * uiScale);
        mGuiCursorY = h / (2.f * uiScale);
    }

    void MouseManager::mouseMoved(const SDLUtil::MouseMotionEvent& arg)
    {
        mBindingsManager->mouseMoved(arg);

        MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        input->setJoystickLastUsed(false);
        input->resetIdleTime();

        if (mGuiCursorEnabled)
        {
            input->setGamepadGuiCursorEnabled(true);

            // We keep track of our own mouse position, so that moving the mouse while in
            // game mode does not move the position of the GUI cursor
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            float uiScale = winMgr->getScalingFactor();
            mGuiCursorX = static_cast<float>(arg.x) / uiScale;
            mGuiCursorY = static_cast<float>(arg.y) / uiScale;

            mMouseWheel = static_cast<int>(arg.z);

            MyGUI::InputManager::getInstance().injectMouseMove(
                static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), mMouseWheel);
            // FIXME: inject twice to force updating focused widget states (tooltips) resulting from changing the
            // viewport by scroll wheel
            MyGUI::InputManager::getInstance().injectMouseMove(
                static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), mMouseWheel);

            winMgr->setCursorActive(true);

            // Check if this movement is from our recent mouse warp
            bool isFromWarp = (mLastWarpX >= 0 && mLastWarpY >= 0 && std::abs(mGuiCursorX - mLastWarpX) < 0.5f
                && std::abs(mGuiCursorY - mLastWarpY) < 0.5f);

            if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible()
                && (std::abs(arg.xrel) > 1 || std::abs(arg.yrel) > 1) && !isFromWarp)
            {
                // Unhide the cursor if it was hidden to show a controller tooltip.
                winMgr->setControllerTooltipVisible(false);
                winMgr->setCursorVisible(true);
            }

            // Clear warp tracking after processing
            mLastWarpX = -1;
            mLastWarpY = -1;
        }

        if (mMouseLookEnabled && !input->controlsDisabled())
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();

            const float cameraSensitivity = Settings::input().mCameraSensitivity;
            float x = arg.xrel * cameraSensitivity * (Settings::input().mInvertXAxis ? -1 : 1) / 256.f;
            float y = arg.yrel * cameraSensitivity * (Settings::input().mInvertYAxis ? -1 : 1)
                * Settings::input().mCameraYMultiplier / 256.f;

            float rot[3];
            rot[0] = -y;
            rot[1] = 0.0f;
            rot[2] = -x;

            // Only actually turn player when we're not in vanity mode
            if (!world->vanityRotateCamera(rot) && input->getControlSwitch("playerlooking"))
            {
                MWWorld::Player& player = world->getPlayer();
                player.yaw(x);
                player.pitch(y);
            }
            else if (!input->getControlSwitch("playerlooking"))
                MWBase::Environment::get().getWorld()->disableDeferredPreviewRotation();
        }
    }

    void MouseManager::mouseReleased(const SDL_MouseButtonEvent& arg, Uint8 id)
    {
        MWBase::Environment::get().getInputManager()->setJoystickLastUsed(false);

        if (mBindingsManager->isDetectingBindingState())
        {
            mBindingsManager->mouseReleased(arg, id);
        }
        else
        {
            bool guiMode = MWBase::Environment::get().getWindowManager()->isGuiMode();
            guiMode = MyGUI::InputManager::getInstance().injectMouseRelease(static_cast<int>(mGuiCursorX),
                          static_cast<int>(mGuiCursorY), SDLUtil::sdlMouseButtonToMyGui(id))
                && guiMode;

            if (mBindingsManager->isDetectingBindingState())
                return; // don't allow same mouseup to bind as initiated bind

            mBindingsManager->setPlayerControlsEnabled(!guiMode);
            mBindingsManager->mouseReleased(arg, id);
        }

        MWBase::Environment::get().getLuaManager()->inputEvent(
            { MWBase::LuaManager::InputEvent::MouseButtonReleased, arg.button });
    }

    void MouseManager::mouseWheelMoved(const SDL_MouseWheelEvent& arg)
    {
        MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        if (mBindingsManager->isDetectingBindingState() || !input->controlsDisabled())
        {
            mBindingsManager->mouseWheelMoved(arg);
        }

        input->setJoystickLastUsed(false);
        MWBase::Environment::get().getLuaManager()->inputEvent({ MWBase::LuaManager::InputEvent::MouseWheel,
            MWBase::LuaManager::InputEvent::WheelChange{ arg.x, arg.y } });
    }

    void MouseManager::mousePressed(const SDL_MouseButtonEvent& arg, Uint8 id)
    {
        MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        input->setJoystickLastUsed(false);
        bool guiMode = false;

        if (id == SDL_BUTTON_MIDDLE && hasFalloutContent())
            logFalloutFocusDiagnostic();

        if (id == SDL_BUTTON_LEFT || id == SDL_BUTTON_RIGHT) // MyGUI only uses these mouse events
        {
            guiMode = MWBase::Environment::get().getWindowManager()->isGuiMode();
            guiMode = MyGUI::InputManager::getInstance().injectMousePress(static_cast<int>(mGuiCursorX),
                          static_cast<int>(mGuiCursorY), SDLUtil::sdlMouseButtonToMyGui(id))
                && guiMode;
            if (MyGUI::InputManager::getInstance().getMouseFocusWidget() != nullptr)
            {
                MyGUI::Button* b
                    = MyGUI::InputManager::getInstance().getMouseFocusWidget()->castType<MyGUI::Button>(false);
                if (b && b->getEnabled() && id == SDL_BUTTON_LEFT)
                {
                    MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
                }
            }
            MWBase::Environment::get().getWindowManager()->setCursorActive(true);
        }

        mBindingsManager->setPlayerControlsEnabled(!guiMode);

        // Don't trigger any mouse bindings while in settings menu, otherwise rebinding controls becomes impossible
        // Also do not trigger bindings when input controls are disabled, e.g. during save loading
        if (!MWBase::Environment::get().getWindowManager()->isSettingsWindowVisible() && !input->controlsDisabled())
        {
            mBindingsManager->mousePressed(arg, id);
        }
        MWBase::Environment::get().getLuaManager()->inputEvent(
            { MWBase::LuaManager::InputEvent::MouseButtonPressed, arg.button });
    }

    void MouseManager::updateCursorMode()
    {
        bool grab = !MWBase::Environment::get().getWindowManager()->containsMode(MWGui::GM_MainMenu)
            && !MWBase::Environment::get().getWindowManager()->isConsoleMode();

        bool wasRelative = mInputWrapper->getMouseRelative();
        bool isRelative = !MWBase::Environment::get().getWindowManager()->isGuiMode();

        // don't keep the pointer away from the window edge in gui mode
        // stop using raw mouse motions and switch to system cursor movements
        mInputWrapper->setMouseRelative(isRelative);

        // we let the mouse escape in the main menu
        mInputWrapper->setGrabPointer(grab && (Settings::input().mGrabCursor || isRelative));

        // we switched to non-relative mode, move our cursor to where the in-game
        // cursor is
        if (!isRelative && wasRelative != isRelative)
        {
            warpMouse();
        }
    }

    void MouseManager::update(float dt)
    {
        SDL_GetRelativeMouseState(&mMouseMoveX, &mMouseMoveY);

        if (!mMouseLookEnabled)
            return;

        float xAxis = mBindingsManager->getActionValue(A_LookLeftRight) * 2.0f - 1.0f;
        float yAxis = mBindingsManager->getActionValue(A_LookUpDown) * 2.0f - 1.0f;
        if (xAxis == 0 && yAxis == 0)
            return;

        const float cameraSensitivity = Settings::input().mCameraSensitivity;
        const float rot[3] = {
            -yAxis * dt * 1000.0f * cameraSensitivity * (Settings::input().mInvertYAxis ? -1 : 1)
                * Settings::input().mCameraYMultiplier / 256.f,
            0.0f,
            -xAxis * dt * 1000.0f * cameraSensitivity * (Settings::input().mInvertXAxis ? -1 : 1) / 256.f,
        };

        // Only actually turn player when we're not in vanity mode
        bool playerLooking = MWBase::Environment::get().getInputManager()->getControlSwitch("playerlooking");
        if (!MWBase::Environment::get().getWorld()->vanityRotateCamera(rot) && playerLooking)
        {
            MWWorld::Player& player = MWBase::Environment::get().getWorld()->getPlayer();
            player.yaw(-rot[2]);
            player.pitch(-rot[0]);
        }
        else if (!playerLooking)
            MWBase::Environment::get().getWorld()->disableDeferredPreviewRotation();

        MWBase::Environment::get().getInputManager()->resetIdleTime();
    }

    bool MouseManager::injectMouseButtonPress(Uint8 button)
    {
        return MyGUI::InputManager::getInstance().injectMousePress(
            static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), SDLUtil::sdlMouseButtonToMyGui(button));
    }

    bool MouseManager::injectMouseButtonRelease(Uint8 button)
    {
        return MyGUI::InputManager::getInstance().injectMouseRelease(
            static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), SDLUtil::sdlMouseButtonToMyGui(button));
    }

    void MouseManager::injectMouseMove(float xMove, float yMove, float mouseWheelMove)
    {
        mGuiCursorX += xMove;
        mGuiCursorY += yMove;
        mMouseWheel += static_cast<int>(mouseWheelMove);

        const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        mGuiCursorX = std::clamp<float>(mGuiCursorX, 0.f, viewSize.width - 1.f);
        mGuiCursorY = std::clamp<float>(mGuiCursorY, 0.f, viewSize.height - 1.f);

        MyGUI::InputManager::getInstance().injectMouseMove(
            static_cast<int>(mGuiCursorX), static_cast<int>(mGuiCursorY), mMouseWheel);
    }

    void MouseManager::warpMouse()
    {
        float guiUiScale = Settings::gui().mScalingFactor;
        mInputWrapper->warpMouse(
            static_cast<int>(mGuiCursorX * guiUiScale), static_cast<int>(mGuiCursorY * guiUiScale));
    }

    void MouseManager::warpMouseToWidget(MyGUI::Widget* widget)
    {
        float widgetX = widget->getAbsoluteCoord().left + widget->getWidth() / 2.f;
        float widgetY = widget->getAbsoluteCoord().top + widget->getHeight() / 4.f;
        if (std::abs(mGuiCursorX - widgetX) > 1 || std::abs(mGuiCursorY - widgetY) > 1)
        {
            mGuiCursorX = widgetX;
            mGuiCursorY = widgetY;
            // Remember where we warped to so we can ignore movement from this warp
            mLastWarpX = widgetX;
            mLastWarpY = widgetY;
            warpMouse();
        }
    }

}
