#include "inputmanagerimp.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <osgViewer/ViewerEventHandlers>

#include <components/debug/debuglog.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/sdlutil/sdlinputwrapper.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/camera.hpp"

#include "../mwworld/esmstore.hpp"
#include "../mwworld/action.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/player.hpp"

#include "actionmanager.hpp"
#include "bindingsmanager.hpp"
#include "controllermanager.hpp"
#include "controlswitch.hpp"
#include "gyromanager.hpp"
#include "keyboardmanager.hpp"
#include "mousemanager.hpp"
#include "sensormanager.hpp"

namespace MWInput
{
    InputManager::InputManager(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer,
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler, const std::filesystem::path& userFile,
        bool userFileExists, const std::filesystem::path& userControllerBindingsFile,
        const std::filesystem::path& controllerBindingsFile, bool grab)
        : mControlsDisabled(false)
        , mInputWrapper(std::make_unique<SDLUtil::InputWrapper>(window, viewer, grab))
        , mBindingsManager(std::make_unique<BindingsManager>(userFile, userFileExists))
        , mControlSwitch(std::make_unique<ControlSwitch>())
        , mActionManager(std::make_unique<ActionManager>(mBindingsManager.get(), viewer, screenCaptureHandler))
        , mKeyboardManager(std::make_unique<KeyboardManager>(mBindingsManager.get()))
        , mMouseManager(std::make_unique<MouseManager>(mBindingsManager.get(), mInputWrapper.get(), window))
        , mControllerManager(std::make_unique<ControllerManager>(
              mBindingsManager.get(), mMouseManager.get(), userControllerBindingsFile, controllerBindingsFile))
        , mSensorManager(std::make_unique<SensorManager>())
        , mGyroManager(std::make_unique<GyroManager>())
    {
        mInputWrapper->setWindowEventCallback(MWBase::Environment::get().getWindowManager());
        mInputWrapper->setKeyboardEventCallback(mKeyboardManager.get());
        mInputWrapper->setMouseEventCallback(mMouseManager.get());
        mInputWrapper->setControllerEventCallback(mControllerManager.get());
        mInputWrapper->setSensorEventCallback(mSensorManager.get());
        loadSelfDriveInputScript();
    }

    void InputManager::clear()
    {
        // Enable all controls
        mControlSwitch->clear();
    }

    InputManager::~InputManager() {}

    void InputManager::update(float dt, bool disableControls, bool disableEvents)
    {
        mControlsDisabled = disableControls;

        mInputWrapper->setMouseVisible(MWBase::Environment::get().getWindowManager()->getCursorVisible());
        mInputWrapper->capture(disableEvents);
        updateSelfDriveInput();

        if (disableControls)
        {
            mMouseManager->updateCursorMode();
            return;
        }

        mBindingsManager->update(dt);

        mMouseManager->updateCursorMode();

        mControllerManager->update(dt);
        mMouseManager->update(dt);
        mSensorManager->update(dt);
        mActionManager->update(dt);

        if (Settings::input().mEnableGyroscope)
        {
            bool controllerAvailable = mControllerManager->isGyroAvailable();
            bool sensorAvailable = mSensorManager->isGyroAvailable();
            if (controllerAvailable || sensorAvailable)
            {
                mGyroManager->update(
                    dt, controllerAvailable ? mControllerManager->getGyroValues() : mSensorManager->getGyroValues());
            }
        }
    }

    void InputManager::loadSelfDriveInputScript()
    {
        const char* pathValue = std::getenv("OPENMW_SELF_DRIVE_INPUT_SCRIPT");
        if (pathValue == nullptr || *pathValue == '\0')
            return;

        std::ifstream stream{ std::filesystem::path(pathValue) };
        if (!stream)
            throw std::runtime_error("Unable to open OPENMW_SELF_DRIVE_INPUT_SCRIPT: " + std::string(pathValue));

        const auto parseUnsigned = [](std::string_view value, int base, std::string_view label) {
            std::uint32_t result = 0;
            const char* begin = value.data();
            const char* end = begin + value.size();
            const auto parsed = std::from_chars(begin, end, result, base);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
                throw std::runtime_error("Invalid self-drive " + std::string(label) + ": " + std::string(value));
            return result;
        };
        const auto controllerButton = [](std::string_view value) -> std::optional<std::uint8_t> {
            constexpr std::array<std::pair<std::string_view, SDL_GameControllerButton>, 6> buttons{ {
                { "A", SDL_CONTROLLER_BUTTON_A },
                { "B", SDL_CONTROLLER_BUTTON_B },
                { "DPadUp", SDL_CONTROLLER_BUTTON_DPAD_UP },
                { "DPadDown", SDL_CONTROLLER_BUTTON_DPAD_DOWN },
                { "DPadLeft", SDL_CONTROLLER_BUTTON_DPAD_LEFT },
                { "DPadRight", SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
            } };
            for (const auto& [name, button] : buttons)
            {
                if (Misc::StringUtils::ciEqual(name, value))
                    return static_cast<std::uint8_t>(button);
            }
            return std::nullopt;
        };

        std::string line;
        std::uint32_t previousTime = 0;
        std::size_t lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            if (line.empty() || line[0] == '#')
                continue;
            std::vector<std::string> fields;
            std::stringstream parser(line);
            std::string field;
            while (std::getline(parser, field, ','))
                fields.push_back(field);
            if (fields.size() < 2 || fields.size() > 5)
                throw std::runtime_error("Malformed self-drive input line " + std::to_string(lineNumber));

            SelfDriveEvent event;
            event.mAtMilliseconds = parseUnsigned(fields[0], 10, "timestamp");
            if (event.mAtMilliseconds > 600000 || (!mSelfDriveEvents.empty() && event.mAtMilliseconds < previousTime))
                throw std::runtime_error("Out-of-order or unbounded self-drive timestamp at line "
                    + std::to_string(lineNumber));
            previousTime = event.mAtMilliseconds;

            if (Misc::StringUtils::ciEqual(fields[1], "controller") && fields.size() == 3)
            {
                const std::optional<std::uint8_t> button = controllerButton(fields[2]);
                if (!button)
                    throw std::runtime_error("Unknown self-drive controller button at line "
                        + std::to_string(lineNumber));
                event.mKind = SelfDriveEventKind::Controller;
                event.mControllerButton = *button;
            }
            else if (Misc::StringUtils::ciEqual(fields[1], "screenshot") && fields.size() == 2)
                event.mKind = SelfDriveEventKind::Screenshot;
            else if (Misc::StringUtils::ciEqual(fields[1], "action") && fields.size() == 3
                && Misc::StringUtils::ciEqual(fields[2], "Activate"))
            {
                event.mKind = SelfDriveEventKind::Action;
                event.mAction = A_Activate;
            }
            else if (Misc::StringUtils::ciEqual(fields[1], "stage-near-form") && fields.size() == 5)
            {
                event.mKind = SelfDriveEventKind::StageNearForm;
                event.mContentFile = fields[2];
                event.mFormIndex = parseUnsigned(fields[3], 16, "FormId index");
                event.mDistance = parseUnsigned(fields[4], 10, "staging distance");
                if (event.mContentFile.empty() || event.mFormIndex > 0x00ffffff || event.mDistance < 48
                    || event.mDistance > 256)
                    throw std::runtime_error("Invalid self-drive stage-near-form target at line "
                        + std::to_string(lineNumber));
            }
            else if (Misc::StringUtils::ciEqual(fields[1], "activate-faced-form") && fields.size() == 4)
            {
                event.mKind = SelfDriveEventKind::ActivateFacedForm;
                event.mContentFile = fields[2];
                event.mFormIndex = parseUnsigned(fields[3], 16, "FormId index");
                event.mAction = A_Activate;
                if (event.mContentFile.empty() || event.mFormIndex > 0x00ffffff)
                    throw std::runtime_error("Invalid self-drive activate-faced-form target at line "
                        + std::to_string(lineNumber));
            }
            else if (Misc::StringUtils::ciEqual(fields[1], "activate-form") && fields.size() == 4)
            {
                event.mKind = SelfDriveEventKind::ActivateForm;
                event.mContentFile = fields[2];
                event.mFormIndex = parseUnsigned(fields[3], 16, "FormId index");
                if (event.mContentFile.empty() || event.mFormIndex > 0x00ffffff)
                    throw std::runtime_error("Invalid self-drive activate-form target at line "
                        + std::to_string(lineNumber));
            }
            else
                throw std::runtime_error("Unknown self-drive command at line " + std::to_string(lineNumber));

            mSelfDriveEvents.push_back(std::move(event));
            if (mSelfDriveEvents.size() > 1024)
                throw std::runtime_error("Self-drive input script exceeds 1024 events");
        }
        if (mSelfDriveEvents.empty())
            throw std::runtime_error("OPENMW_SELF_DRIVE_INPUT_SCRIPT contains no events");
        mSelfDriveStarted = std::chrono::steady_clock::now();
        Log(Debug::Info) << "Self-drive input: loaded path=" << pathValue << " events=" << mSelfDriveEvents.size();
    }

    void InputManager::maintainSelfDriveFacing()
    {
        if (!mSelfDriveFacingRef)
            return;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr target;
        try
        {
            target = world->getPtr(ESM::RefId(*mSelfDriveFacingRef), false);
        }
        catch (const std::runtime_error&)
        {
            return;
        }
        const MWWorld::Ptr player = world->getPlayerPtr();
        MWRender::Camera* camera = world->getCamera();
        if (target.isEmpty() || player.isEmpty() || !target.isInCell() || !player.isInCell()
            || target.getCell() != player.getCell() || camera == nullptr)
            return;

        camera->updateCamera();
        const osg::Vec3d cameraPosition = osg::Matrixd::inverse(camera->getViewMatrix()).getTrans();
        osg::Vec3d targetFocus(target.getRefData().getPosition().asVec3());
        targetFocus.z() += std::max(24.f, world->getHalfExtents(target, true).z());
        const osg::Vec3d aim = targetFocus - cameraPosition;
        const float horizontal = std::hypot(aim.x(), aim.y());
        if (horizontal < 1.f)
            return;
        const float yaw = std::atan2(static_cast<float>(aim.x()), static_cast<float>(aim.y()));
        world->rotateObject(player,
            osg::Vec3f(player.getRefData().getPosition().rot[0], 0.f, yaw), MWBase::RotationFlag_none);
        camera->setYaw(-yaw, true);
        camera->setPitch(std::atan2(static_cast<float>(aim.z()), horizontal), true);
        camera->setRoll(0.f);
        camera->updateCamera();
    }

    void InputManager::updateSelfDriveInput()
    {
        maintainSelfDriveFacing();
        if (mNextSelfDriveEvent >= mSelfDriveEvents.size())
            return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - mSelfDriveStarted);
        while (mNextSelfDriveEvent < mSelfDriveEvents.size()
            && elapsed.count() >= mSelfDriveEvents[mNextSelfDriveEvent].mAtMilliseconds)
        {
            const std::size_t eventIndex = mNextSelfDriveEvent++;
            const SelfDriveEvent event = mSelfDriveEvents[eventIndex];
            const auto logExecution = [&] {
                Log(Debug::Info) << "Self-drive input: execute index=" << eventIndex
                                 << " atMs=" << event.mAtMilliseconds << " kind=" << static_cast<int>(event.mKind);
            };
            if (event.mKind == SelfDriveEventKind::Controller)
            {
                logExecution();
                SDL_ControllerButtonEvent button{};
                button.type = SDL_CONTROLLERBUTTONDOWN;
                button.button = event.mControllerButton;
                button.state = SDL_PRESSED;
                mControllerManager->buttonPressed(0, button);
                button.type = SDL_CONTROLLERBUTTONUP;
                button.state = SDL_RELEASED;
                mControllerManager->buttonReleased(0, button);
            }
            else if (event.mKind == SelfDriveEventKind::Screenshot)
            {
                logExecution();
                mActionManager->executeAction(A_Screenshot);
            }
            else if (event.mKind == SelfDriveEventKind::Action)
            {
                logExecution();
                MWBase::World* world = MWBase::Environment::get().getWorld();
                const MWWorld::Ptr player = world->getPlayerPtr();
                const MWWorld::Ptr faced = world->getFacedObject();
                std::ostringstream facedId;
                if (faced.isEmpty())
                    facedId << "<none>";
                else
                    facedId << faced.getCellRef().getRefNum();
                Log(Debug::Info) << "Self-drive input: dispatch ordinary action=Activate faced="
                                 << facedId.str()
                                 << " type=" << (faced.isEmpty() ? 0 : faced.getType())
                                 << " distance="
                                 << (faced.isEmpty() || player.isEmpty()
                                             ? -1.f
                                             : (faced.getRefData().getPosition().asVec3()
                                                   - player.getRefData().getPosition().asVec3())
                                                   .length());
                mActionManager->executeAction(event.mAction);
            }
            else
            {
                const std::vector<std::string>& contentFiles
                    = MWBase::Environment::get().getWorld()->getContentFiles();
                std::optional<int> loadOrderContentFile;
                std::optional<int> pluginContentFile;
                int pluginIndex = 0;
                for (std::size_t index = 0; index < contentFiles.size(); ++index)
                {
                    if (Misc::StringUtils::ciEqual(contentFiles[index], event.mContentFile))
                    {
                        if (loadOrderContentFile)
                            throw std::runtime_error("Ambiguous self-drive content file: " + event.mContentFile);
                        loadOrderContentFile = static_cast<int>(index);
                        pluginContentFile = pluginIndex;
                    }
                    if (Misc::StringUtils::ciEndsWith(contentFiles[index], ".esm")
                        || Misc::StringUtils::ciEndsWith(contentFiles[index], ".esp")
                        || Misc::StringUtils::ciEndsWith(contentFiles[index], ".omwgame")
                        || Misc::StringUtils::ciEndsWith(contentFiles[index], ".omwaddon")
                        || Misc::StringUtils::ciEndsWith(contentFiles[index], ".project"))
                        ++pluginIndex;
                }
                if (!loadOrderContentFile || !pluginContentFile)
                    throw std::runtime_error("Missing self-drive content file: " + event.mContentFile);
                MWBase::World* world = MWBase::Environment::get().getWorld();
                const MWWorld::Ptr actor = world->getPlayerPtr();
                MWWorld::Ptr target;
                std::optional<int> resolvedContentFile;
                const auto findActive = [&](int contentFileIndex) {
                    try
                    {
                        MWWorld::Ptr candidate = world->getPtr(
                            ESM::RefId(ESM::FormId{ event.mFormIndex, contentFileIndex }), false);
                        if (!candidate.isInCell() || !actor.isInCell() || candidate.getCell() != actor.getCell())
                            return MWWorld::Ptr{};
                        return candidate;
                    }
                    catch (const std::runtime_error&)
                    {
                        return MWWorld::Ptr{};
                    }
                };
                const std::array<int, 2> candidates{ *loadOrderContentFile, *pluginContentFile };
                for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
                {
                    if (candidateIndex != 0 && candidates[candidateIndex] == candidates[0])
                        continue;
                    MWWorld::Ptr candidate = findActive(candidates[candidateIndex]);
                    if (!candidate.isEmpty())
                    {
                        if (!target.isEmpty())
                            throw std::runtime_error("Ambiguous self-drive placed FormId: "
                                + event.mContentFile);
                        target = candidate;
                        resolvedContentFile = candidates[candidateIndex];
                    }
                }
                if ((target.isEmpty() || !resolvedContentFile)
                    && (event.mKind == SelfDriveEventKind::StageNearForm
                        || event.mKind == SelfDriveEventKind::ActivateFacedForm)
                    && elapsed.count() < static_cast<std::int64_t>(event.mAtMilliseconds) + 30000)
                {
                    --mNextSelfDriveEvent;
                    break;
                }
                if (target.isEmpty() || !resolvedContentFile)
                    throw std::runtime_error("Missing active self-drive placed FormId: " + event.mContentFile);
                if (event.mKind == SelfDriveEventKind::StageNearForm)
                {
                    logExecution();
                    const std::uint32_t waitDelay = elapsed.count() > event.mAtMilliseconds
                        ? static_cast<std::uint32_t>(elapsed.count() - event.mAtMilliseconds)
                        : 0;
                    for (std::size_t future = mNextSelfDriveEvent; future < mSelfDriveEvents.size(); ++future)
                    {
                        if (mSelfDriveEvents[future].mAtMilliseconds > 600000 - waitDelay)
                            throw std::runtime_error("Self-drive staging wait exceeds bounded event timeline");
                        mSelfDriveEvents[future].mAtMilliseconds += waitDelay;
                    }
                    const osg::Vec3f targetPosition = target.getRefData().getPosition().asVec3();
                    // A placed activator's authored Z rotation owns its usable front. Deriving the approach vector
                    // from the diagnostic-start player's arbitrary location can put the actor at a side or rear
                    // quarter even though the activation ray happens to intersect the mesh.
                    const float targetYaw = target.getRefData().getPosition().rot[2];
                    osg::Vec3f outward(std::sin(targetYaw), std::cos(targetYaw), 0.f);
                    osg::Vec3f stagedPosition = targetPosition + outward * static_cast<float>(event.mDistance);
                    stagedPosition.z() = targetPosition.z();
                    MWWorld::Ptr stagedActor = world->moveObject(actor, stagedPosition, true, true);
                    mSelfDriveFacingRef = target.getCellRef().getRefNum();
                    const osg::Vec3f aimDelta = targetPosition - stagedPosition;
                    const float yaw = std::atan2(aimDelta.x(), aimDelta.y());
                    world->rotateObject(stagedActor, osg::Vec3f(0.f, 0.f, yaw), MWBase::RotationFlag_none);
                    if (MWRender::Camera* camera = world->getCamera())
                    {
                        camera->setYaw(-yaw, true);
                        camera->setPitch(0.f, true);
                        camera->instantTransition();
                        camera->updateCamera();

                        const osg::Vec3d cameraPosition
                            = osg::Matrixd::inverse(camera->getViewMatrix()).getTrans();
                        osg::Vec3d targetFocus(targetPosition);
                        const osg::Vec3f targetHalfExtents = world->getHalfExtents(target, true);
                        targetFocus.z() += std::max(24.f, targetHalfExtents.z());
                        const osg::Vec3d cameraAim = targetFocus - cameraPosition;
                        const float cameraPitch
                            = std::atan2(cameraAim.z(), std::hypot(cameraAim.x(), cameraAim.y()));
                        MWWorld::Player& playerController = world->getPlayer();
                        playerController.pitch(camera->getPitch() - cameraPitch);
                        camera->setPitch(cameraPitch, true);
                        camera->instantTransition();
                        camera->updateCamera();
                        Log(Debug::Info) << "Self-drive input: aimed real camera at staged form camera=("
                                         << cameraPosition.x() << "," << cameraPosition.y() << ","
                                         << cameraPosition.z() << ") focus=(" << targetFocus.x() << ","
                                         << targetFocus.y() << "," << targetFocus.z() << ") halfExtents=("
                                         << targetHalfExtents.x() << "," << targetHalfExtents.y() << ","
                                         << targetHalfExtents.z() << ")";
                    }
                    Log(Debug::Info) << "Self-drive input: staged near form without activation file="
                                     << event.mContentFile << " index=0x" << std::hex << event.mFormIndex
                                     << std::dec << " waitMs=" << waitDelay << " distance=" << event.mDistance
                                     << " player=("
                                     << stagedPosition.x() << "," << stagedPosition.y() << ","
                                     << stagedPosition.z() << ") target=" << target.toString()
                                     << " authoredFrontYaw=" << targetYaw << " maintainFacingUntilActivate=1";
                    continue;
                }
                if (event.mKind == SelfDriveEventKind::ActivateFacedForm)
                {
                    const MWWorld::Ptr faced = world->getFacedObject();
                    const bool targetFaced = !faced.isEmpty() && faced.getType() == target.getType()
                        && faced.getCellRef().getRefId() == target.getCellRef().getRefId();
                    if (!targetFaced
                        && elapsed.count() < static_cast<std::int64_t>(event.mAtMilliseconds) + 10000)
                    {
                        --mNextSelfDriveEvent;
                        break;
                    }
                    if (!targetFaced)
                        throw std::runtime_error("Self-drive ordinary Activate never faced expected TERM base: "
                            + event.mContentFile + " faced=" + faced.toString() + " target=" + target.toString());

                    logExecution();
                    const std::uint32_t waitDelay = elapsed.count() > event.mAtMilliseconds
                        ? static_cast<std::uint32_t>(elapsed.count() - event.mAtMilliseconds)
                        : 0;
                    for (std::size_t future = mNextSelfDriveEvent; future < mSelfDriveEvents.size(); ++future)
                    {
                        if (mSelfDriveEvents[future].mAtMilliseconds > 600000 - waitDelay)
                            throw std::runtime_error("Self-drive faced-object wait exceeds bounded event timeline");
                        mSelfDriveEvents[future].mAtMilliseconds += waitDelay;
                    }
                    std::ostringstream facedId;
                    facedId << faced.getCellRef().getRefNum();
                    Log(Debug::Info) << "Self-drive input: dispatch ordinary action=Activate faced="
                                     << facedId.str() << " base=" << faced.getCellRef().getRefId().toDebugString()
                                     << " type=" << faced.getType()
                                     << " distance="
                                     << (faced.getRefData().getPosition().asVec3()
                                            - actor.getRefData().getPosition().asVec3())
                                            .length()
                                     << " expectedFile=" << event.mContentFile << " expectedIndex=0x"
                                     << std::hex << event.mFormIndex << std::dec << " waitMs=" << waitDelay;
                    mSelfDriveFacingRef.reset();
                    mActionManager->executeAction(event.mAction);
                    continue;
                }
                logExecution();
                Log(Debug::Info) << "Self-drive input: resolved activate-form file=" << event.mContentFile
                                 << " index=0x" << std::hex << event.mFormIndex << std::dec
                                 << " contentFile=" << *resolvedContentFile;
                std::unique_ptr<MWWorld::Action> action = target.getClass().activate(target, actor);
                if (!action)
                    throw std::runtime_error("Self-drive activate-form returned no action");
                action->execute(actor);
            }
        }
    }

    void InputManager::setDragDrop(bool dragDrop)
    {
        mBindingsManager->setDragDrop(dragDrop);
    }

    void InputManager::setGamepadGuiCursorEnabled(bool enabled)
    {
        mControllerManager->setGamepadGuiCursorEnabled(enabled);
    }

    bool InputManager::isGamepadGuiCursorEnabled()
    {
        return mControllerManager->gamepadGuiCursorEnabled();
    }

    void InputManager::changeInputMode(bool guiMode)
    {
        mControllerManager->setGuiCursorEnabled(guiMode);
        mMouseManager->setGuiCursorEnabled(guiMode);
        mGyroManager->setGuiCursorEnabled(guiMode);
        mMouseManager->setMouseLookEnabled(!guiMode);
        if (guiMode)
            MWBase::Environment::get().getWindowManager()->showCrosshair(false);

        bool isCursorVisible
            = guiMode && (!mControllerManager->joystickLastUsed() || mControllerManager->gamepadGuiCursorEnabled());
        MWBase::Environment::get().getWindowManager()->setCursorVisible(isCursorVisible);
        // if not in gui mode, the camera decides whether to show crosshair or not.
    }

    void InputManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        mSensorManager->processChangedSettings(changed);
    }

    bool InputManager::getControlSwitch(std::string_view sw)
    {
        return mControlSwitch->get(sw);
    }

    void InputManager::toggleControlSwitch(std::string_view sw, bool value)
    {
        mControlSwitch->set(sw, value);
    }

    void InputManager::resetIdleTime()
    {
        mActionManager->resetIdleTime();
    }

    bool InputManager::isIdle() const
    {
        return mActionManager->getIdleTime() > 0.5;
    }

    std::string_view InputManager::getActionDescription(int action) const
    {
        return mBindingsManager->getActionDescription(action);
    }

    std::string InputManager::getActionKeyBindingName(int action) const
    {
        return mBindingsManager->getActionKeyBindingName(action);
    }

    std::string InputManager::getActionControllerBindingName(int action) const
    {
        return mBindingsManager->getActionControllerBindingName(action);
    }

    bool InputManager::actionIsActive(int action) const
    {
        return mBindingsManager->actionIsActive(action);
    }

    float InputManager::getActionValue(int action) const
    {
        return mBindingsManager->getActionValue(action);
    }

    bool InputManager::isControllerButtonPressed(SDL_GameControllerButton button) const
    {
        return mControllerManager->isButtonPressed(button);
    }

    float InputManager::getControllerAxisValue(SDL_GameControllerAxis axis) const
    {
        return mControllerManager->getAxisValue(axis);
    }

    int InputManager::getMouseMoveX() const
    {
        return mMouseManager->getMouseMoveX();
    }

    int InputManager::getMouseMoveY() const
    {
        return mMouseManager->getMouseMoveY();
    }

    void InputManager::warpMouseToWidget(MyGUI::Widget* widget)
    {
        mMouseManager->warpMouseToWidget(widget);
        mMouseManager->injectMouseMove(1, 0, 0);
        MWBase::Environment::get().getWindowManager()->setCursorActive(true);
    }

    const std::initializer_list<int>& InputManager::getActionKeySorting()
    {
        return mBindingsManager->getActionKeySorting();
    }

    const std::initializer_list<int>& InputManager::getActionControllerSorting()
    {
        return mBindingsManager->getActionControllerSorting();
    }

    void InputManager::enableDetectingBindingMode(int action, bool keyboard)
    {
        mBindingsManager->enableDetectingBindingMode(action, keyboard);
    }

    int InputManager::countSavedGameRecords() const
    {
        return mControlSwitch->countSavedGameRecords();
    }

    void InputManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        mControlSwitch->write(writer, progress);
    }

    void InputManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_INPU)
        {
            mControlSwitch->readRecord(reader, type);
        }
    }

    void InputManager::resetToDefaultKeyBindings()
    {
        mBindingsManager->loadKeyDefaults(true);
    }

    void InputManager::resetToDefaultControllerBindings()
    {
        mBindingsManager->loadControllerDefaults(true);
    }

    void InputManager::setJoystickLastUsed(bool enabled)
    {
        mControllerManager->setJoystickLastUsed(enabled);
    }

    bool InputManager::joystickLastUsed()
    {
        return mControllerManager->joystickLastUsed();
    }

    std::string InputManager::getControllerButtonIcon(int button)
    {
        return mControllerManager->getControllerButtonIcon(button);
    }

    std::string InputManager::getControllerAxisIcon(int axis)
    {
        return mControllerManager->getControllerAxisIcon(axis);
    }

    void InputManager::executeAction(int action)
    {
        mActionManager->executeAction(action);
    }

    void InputManager::injectEscapeKey() 
    {
        SDL_KeyboardEvent arg = {};
        arg.type = SDL_KEYDOWN;
        arg.keysym.sym = SDLK_ESCAPE;
        arg.keysym.scancode = SDL_SCANCODE_ESCAPE;
        arg.repeat = false;
        mKeyboardManager->keyPressed(arg);
        mKeyboardManager->keyReleased(arg);
    }

    void InputManager::saveBindings()
    {
        mBindingsManager->saveBindings();
    }
}
