#ifndef MWINPUT_MWINPUTMANAGERIMP_H
#define MWINPUT_MWINPUTMANAGERIMP_H

#include <memory>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <osg/ref_ptr>
#include <osgViewer/ViewerEventHandlers>

#include <components/sdlutil/events.hpp>
#include <components/settings/settings.hpp>
#include <components/esm/formid.hpp>
#include <filesystem>

#include "../mwbase/inputmanager.hpp"

#include "actions.hpp"

namespace MWWorld
{
    class Player;
}

namespace MWBase
{
    class WindowManager;
}

namespace SDLUtil
{
    class InputWrapper;
}

struct SDL_Window;

namespace MWInput
{
    class ControlSwitch;
    class ActionManager;
    class BindingsManager;
    class ControllerManager;
    class KeyboardManager;
    class MouseManager;
    class SensorManager;
    class GyroManager;

    /**
     * @brief Class that provides a high-level API for game input
     */
//## VR_PATCH BEGIN
// the vr input manager needs to derive this.
    class InputManager : public MWBase::InputManager
//## VR_PATCH END
    {
    public:
        InputManager(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer,
            osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler, const std::filesystem::path& userFile,
            bool userFileExists, const std::filesystem::path& userControllerBindingsFile,
            const std::filesystem::path& controllerBindingsFile, bool grab);

//## VR_PATCH BEGIN
        virtual ~InputManager();
//## VR_PATCH END

        /// Clear all savegame-specific data
        void clear() override;

        void update(float dt, bool disableControls, bool disableEvents = false) override;

        void changeInputMode(bool guiMode) override;

        void processChangedSettings(const Settings::CategorySettingVector& changed) override;

        void setDragDrop(bool dragDrop) override;
        void setGamepadGuiCursorEnabled(bool enabled) override;
        bool isGamepadGuiCursorEnabled() override;

        void toggleControlSwitch(std::string_view sw, bool value) override;
        bool getControlSwitch(std::string_view sw) override;

        std::string_view getActionDescription(int action) const override;
        std::string getActionKeyBindingName(int action) const override;
        std::string getActionControllerBindingName(int action) const override;
        bool actionIsActive(int action) const override;

        float getActionValue(int action) const override;
        bool isControllerButtonPressed(SDL_GameControllerButton button) const override;
        float getControllerAxisValue(SDL_GameControllerAxis axis) const override;
        int getMouseMoveX() const override;
        int getMouseMoveY() const override;
        void warpMouseToWidget(MyGUI::Widget* widget) override;

        int getNumActions() override { return A_Last; }
        const std::initializer_list<int>& getActionKeySorting() override;
        const std::initializer_list<int>& getActionControllerSorting() override;
        void enableDetectingBindingMode(int action, bool keyboard) override;
        void resetToDefaultKeyBindings() override;
        void resetToDefaultControllerBindings() override;

        void setJoystickLastUsed(bool enabled) override;
        bool joystickLastUsed() override;
        std::string getControllerButtonIcon(int button) override;
        std::string getControllerAxisIcon(int axis) override;

        int countSavedGameRecords() const override;
        void write(ESM::ESMWriter& writer, Loading::Listener& progress) override;
        void readRecord(ESM::ESMReader& reader, uint32_t type) override;

        void resetIdleTime() override;
        bool isIdle() const override;

        void executeAction(int action) override;

        bool controlsDisabled() override { return mControlsDisabled; }
//## VR_PATCH BEGIN
        void applyHapticsLeftHand(float intensity) override{}
        void applyHapticsRightHand(float intensity) override{}

        void injectEscapeKey() override;

    protected:
//## VR_PATCH END
        bool mControlsDisabled;

        enum class SelfDriveEventKind
        {
            Controller,
            Screenshot,
            Action,
            StageNearForm,
            ActivateFacedForm,
            ActivateForm,
        };

        struct SelfDriveEvent
        {
            std::uint32_t mAtMilliseconds = 0;
            SelfDriveEventKind mKind = SelfDriveEventKind::Controller;
            std::uint8_t mControllerButton = 0;
            int mAction = 0;
            std::string mContentFile;
            std::uint32_t mFormIndex = 0;
            std::uint32_t mDistance = 0;
        };

        void loadSelfDriveInputScript();
        void maintainSelfDriveFacing();
        void updateSelfDriveInput();

        std::vector<SelfDriveEvent> mSelfDriveEvents;
        std::size_t mNextSelfDriveEvent = 0;
        std::chrono::steady_clock::time_point mSelfDriveStarted;
        std::optional<ESM::FormId> mSelfDriveFacingRef;

        void saveBindings() override;

        std::unique_ptr<SDLUtil::InputWrapper> mInputWrapper;
        std::unique_ptr<BindingsManager> mBindingsManager;
        std::unique_ptr<ControlSwitch> mControlSwitch;
        std::unique_ptr<ActionManager> mActionManager;
        std::unique_ptr<KeyboardManager> mKeyboardManager;
        std::unique_ptr<MouseManager> mMouseManager;
        std::unique_ptr<ControllerManager> mControllerManager;
        std::unique_ptr<SensorManager> mSensorManager;
        std::unique_ptr<GyroManager> mGyroManager;
    };
}
#endif
