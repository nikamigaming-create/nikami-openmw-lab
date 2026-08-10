#ifndef MWGUI_MESSAGE_BOX_H
#define MWGUI_MESSAGE_BOX_H

#include <memory>

#include "windowbase.hpp"

namespace MyGUI
{
    class Widget;
    class Button;
    class EditBox;
}

namespace MWGui
{
    struct FnvMenuXmlDocument;
    struct FnvHackingMenuPresentation;
    class InteractiveMessageBox;
    class MessageBoxManager;
    class MessageBox;
    class MessageBoxManager
    {
    public:
        MessageBoxManager(float timePerChar);
        ~MessageBoxManager();
        void onFrame(float frameDuration);
        void createMessageBox(std::string_view message, bool stat = false);
        void removeStaticMessageBox();
        bool createInteractiveMessageBox(std::string_view message, const std::vector<std::string>& buttons,
            bool immediate = false, int defaultFocus = -1);
        bool createInteractiveFnvMenuMessageBox(const FnvMenuXmlDocument& menu, std::string_view frameTile,
            std::string_view messageTile, std::string_view buttonTile, std::string_view message,
            const std::vector<std::string>& buttons, bool immediate = false, int defaultFocus = -1,
            const FnvHackingMenuPresentation* hacking = nullptr);
        bool isInteractiveMessageBox();

        std::size_t getMessagesCount();

        const InteractiveMessageBox* getInteractiveMessageBox() const { return mInterMessageBoxe.get(); }

        /// Remove all message boxes
        void clear();

        bool removeMessageBox(MessageBox* msgbox);

        /// @param reset Reset the pressed button to -1 after reading it.
        int readPressedButton(bool reset = true);

        void resetInteractiveMessageBox();

        void setLastButtonPressed(int index);

        typedef MyGUI::delegates::MultiDelegate<int> EventHandle_Int;

        // Note: this delegate unassigns itself after it was fired, i.e. works once.
        EventHandle_Int eventButtonPressed;

        void onButtonPressed(int button)
        {
            eventButtonPressed(button);
            eventButtonPressed.clear();
        }

        void setVisible(bool value);

        const std::vector<std::unique_ptr<MessageBox>>& getActiveMessageBoxes() const;

    private:
        std::vector<std::unique_ptr<MessageBox>> mMessageBoxes;
        std::unique_ptr<InteractiveMessageBox> mInterMessageBoxe;
        MessageBox* mStaticMessageBox;
        float mMessageBoxSpeed;
        int mLastButtonPressed;
        bool mVisible = true;
    };

    class MessageBox : public Layout
    {
    public:
        MessageBox(MessageBoxManager& parMessageBoxManager, std::string_view message);
        const std::string& getMessage() { return mMessage; }
        int getHeight();
        void update(int height);
//## VR_PATCH BEGIN
// Rewrote handling of setVisible, now handled by base class.
        ~MessageBox();
//        void setVisible(bool value);
//## VR_PATCH END
        float mCurrentTime;
        float mMaxTime;

    protected:
        MessageBoxManager& mMessageBoxManager;
        std::string mMessage;
        MyGUI::EditBox* mMessageWidget;
        int mBottomPadding;
        int mNextBoxPadding;
    };

    class InteractiveMessageBox : public WindowModal
    {
    public:
        InteractiveMessageBox(MessageBoxManager& parMessageBoxManager, const std::string& message,
            const std::vector<std::string>& buttons, bool immediate, size_t defaultFocus,
            const FnvMenuXmlDocument* fnvMenu = nullptr, std::string_view frameTile = {},
            std::string_view messageTile = {}, std::string_view buttonTile = {},
            const FnvHackingMenuPresentation* hacking = nullptr);
        void mousePressed(MyGUI::Widget* widget);
        int readPressedButton();

        MyGUI::Widget* getDefaultKeyFocus() override;

        bool exit() override;

        void closeDefault();

        bool mMarkedToDelete;

        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

    private:
        void buttonActivated(MyGUI::Widget* widget);
        void mouseMoved(MyGUI::Widget* widget, int left, int top);
        void setHackingFocus(std::size_t buttonIndex);
        void finish(int result);

        MessageBoxManager& mMessageBoxManager;
        MyGUI::EditBox* mMessageWidget;
        MyGUI::Widget* mButtonsWidget;
        std::vector<MyGUI::Button*> mButtons;
        std::vector<int> mButtonValues;
        std::vector<MyGUI::EditBox*> mHackingLabels;
        std::vector<std::size_t> mHackingControllerButtons;

        int mButtonPressed;
        size_t mDefaultFocus;
        bool mImmediate;
        bool mHacking;
        size_t mControllerFocus = 0;
    };

}

#endif
