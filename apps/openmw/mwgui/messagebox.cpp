#include "messagebox.hpp"

#include <algorithm>
#include <optional>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_UString.h>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "fnvmenuxml.hpp"

namespace MWGui
{

    MessageBoxManager::MessageBoxManager(float timePerChar)
    {
        mStaticMessageBox = nullptr;
        mLastButtonPressed = -1;
        mMessageBoxSpeed = timePerChar;
    }

    MessageBoxManager::~MessageBoxManager()
    {
        MessageBoxManager::clear();
    }

    std::size_t MessageBoxManager::getMessagesCount()
    {
        return mMessageBoxes.size();
    }

    void MessageBoxManager::clear()
    {
        if (mInterMessageBoxe)
        {
            mInterMessageBoxe->setVisible(false);
            mInterMessageBoxe.reset();
        }

        mMessageBoxes.clear();
        mStaticMessageBox = nullptr;

        mLastButtonPressed = -1;
    }

    void MessageBoxManager::resetInteractiveMessageBox()
    {
        if (mInterMessageBoxe)
        {
            mInterMessageBoxe->setVisible(false);
            mInterMessageBoxe.reset();
        }
    }

    void MessageBoxManager::setLastButtonPressed(int index)
    {
        mLastButtonPressed = index;
    }

    void MessageBoxManager::onFrame(float frameDuration)
    {
        for (auto it = mMessageBoxes.begin(); it != mMessageBoxes.end();)
        {
            (*it)->mCurrentTime += frameDuration;
            if ((*it)->mCurrentTime >= (*it)->mMaxTime && it->get() != mStaticMessageBox)
            {
                it = mMessageBoxes.erase(it);
            }
            else
                ++it;
        }

        float height = 0;
        auto it = mMessageBoxes.begin();
        while (it != mMessageBoxes.end())
        {
            (*it)->update(static_cast<int>(height));
            height += (*it)->getHeight();
            ++it;
        }

        if (mInterMessageBoxe != nullptr && mInterMessageBoxe->mMarkedToDelete)
        {
            mLastButtonPressed = mInterMessageBoxe->readPressedButton();
            mInterMessageBoxe->setVisible(false);
            mInterMessageBoxe.reset();
            MWBase::Environment::get().getInputManager()->changeInputMode(
                MWBase::Environment::get().getWindowManager()->isGuiMode());
        }
    }

    void MessageBoxManager::createMessageBox(std::string_view message, bool stat)
    {
        auto box = std::make_unique<MessageBox>(*this, message);
        box->mCurrentTime = 0;
        auto realMessage = MyGUI::LanguageManager::getInstance().replaceTags({ message.data(), message.size() });
        box->mMaxTime = realMessage.length() * mMessageBoxSpeed;

        if (stat)
            mStaticMessageBox = box.get();

        box->setVisible(mVisible);

        mMessageBoxes.push_back(std::move(box));

        if (mMessageBoxes.size() > 3)
        {
            mMessageBoxes.erase(mMessageBoxes.begin());
        }

        int height = 0;
        for (const auto& messageBox : mMessageBoxes)
        {
            messageBox->update(height);
            height += messageBox->getHeight();
        }
//## VR_PATCH BEGIN
// Make sure message boxes become visible to VR.
        mMessageBoxes.back()->setVisible(true);
//## VR_PATCH END
    }

    void MessageBoxManager::removeStaticMessageBox()
    {
        removeMessageBox(mStaticMessageBox);
        mStaticMessageBox = nullptr;
    }

    bool MessageBoxManager::createInteractiveMessageBox(
        std::string_view message, const std::vector<std::string>& buttons, bool immediate, int defaultFocus)
    {
        if (mInterMessageBoxe != nullptr)
        {
            Log(Debug::Warning) << "Warning: replacing an interactive message box that was not answered yet";
            mInterMessageBoxe->setVisible(false);
        }

        mInterMessageBoxe
            = std::make_unique<InteractiveMessageBox>(*this, std::string{ message }, buttons, immediate, defaultFocus);
        mLastButtonPressed = -1;

        return true;
    }

    bool MessageBoxManager::createInteractiveFnvMenuMessageBox(const FnvMenuXmlDocument& menu,
        std::string_view frameTile, std::string_view messageTile, std::string_view buttonTile,
        std::string_view message, const std::vector<std::string>& buttons, bool immediate, int defaultFocus,
        const FnvHackingMenuPresentation* hacking)
    {
        if (mInterMessageBoxe != nullptr)
        {
            Log(Debug::Warning) << "Warning: replacing an interactive message box that was not answered yet";
            mInterMessageBoxe->setVisible(false);
        }
        mInterMessageBoxe = std::make_unique<InteractiveMessageBox>(*this, std::string(message), buttons,
            immediate, defaultFocus, &menu, frameTile, messageTile, buttonTile, hacking);
        mLastButtonPressed = -1;
        return true;
    }

    bool MessageBoxManager::isInteractiveMessageBox()
    {
        return mInterMessageBoxe != nullptr;
    }

    bool MessageBoxManager::removeMessageBox(MessageBox* msgbox)
    {
        for (auto it = mMessageBoxes.begin(); it != mMessageBoxes.end(); ++it)
        {
            if (it->get() == msgbox)
            {
                mMessageBoxes.erase(it);
                return true;
            }
        }
        return false;
    }

    const std::vector<std::unique_ptr<MessageBox>>& MessageBoxManager::getActiveMessageBoxes() const
    {
        return mMessageBoxes;
    }

    int MessageBoxManager::readPressedButton(bool reset)
    {
        int pressed = mLastButtonPressed;
        if (reset)
            mLastButtonPressed = -1;
        return pressed;
    }

    void MessageBoxManager::setVisible(bool value)
    {
        mVisible = value;
        for (const auto& messageBox : mMessageBoxes)
            messageBox->setVisible(value);
    }

    MessageBox::MessageBox(MessageBoxManager& parMessageBoxManager, std::string_view message)
        : Layout("openmw_messagebox.layout")
        , mCurrentTime(0)
        , mMaxTime(0)
        , mMessageBoxManager(parMessageBoxManager)
        , mMessage(message)
    {
        // defines
        mBottomPadding = 48;
        mNextBoxPadding = 4;

        getWidget(mMessageWidget, "message");

        mMessageWidget->setCaptionWithReplacing(mMessage);
    }

//## VR_PATCH BEGIN
// Make sure message boxes are hidden from VR on destruction
    MessageBox::~MessageBox()
    {
        setVisible(false);
    }

//## VR_PATCH END
    void MessageBox::update(int height)
    {
        MyGUI::IntSize gameWindowSize = MyGUI::RenderManager::getInstance().getViewSize();
        MyGUI::IntPoint pos;
        pos.left = (gameWindowSize.width - mMainWidget->getWidth()) / 2;
        pos.top = (gameWindowSize.height - mMainWidget->getHeight() - height - mBottomPadding);

        mMainWidget->setPosition(pos);
    }

    int MessageBox::getHeight()
    {
        return mMainWidget->getHeight() + mNextBoxPadding;
    }

//## VR_PATCH BEGIN
// Do not override setVisible
//    void MessageBox::setVisible(bool value)
//    {
//        mMainWidget->setVisible(value);
//    }
//## VR_PATCH END

    InteractiveMessageBox::InteractiveMessageBox(MessageBoxManager& parMessageBoxManager, const std::string& message,
        const std::vector<std::string>& buttons, bool immediate, size_t defaultFocus,
        const FnvMenuXmlDocument* fnvMenu, std::string_view frameTile, std::string_view messageTile,
        std::string_view buttonTile, const FnvHackingMenuPresentation* hacking)
        : WindowModal(fnvMenu != nullptr ? "openmw_fnv_menu.layout"
                                        : (MWBase::Environment::get().getWindowManager()->isGuiMode()
                                                  ? "openmw_interactive_messagebox_notransp.layout"
                                                  : "openmw_interactive_messagebox.layout"))
        , mMessageBoxManager(parMessageBoxManager)
        , mButtonPressed(-1)
        , mDefaultFocus(defaultFocus)
        , mImmediate(immediate)
        , mHacking(hacking != nullptr)
        , mControllerFocus(0)
    {
        int textPadding = 10; // padding between text-widget and main-widget
        int textButtonPadding = 10; // padding between the text-widget und the button-widget
        int buttonLeftPadding = 10; // padding between the buttons if horizontal
        int buttonTopPadding = 10; // ^-- if vertical
        int buttonLabelLeftPadding = 12; // padding between button label and button itself, from left
        int buttonLabelTopPadding = 4; // padding between button label and button itself, from top
        int buttonMainPadding = 10; // padding between buttons and bottom of the main widget

        mMarkedToDelete = false;

        getWidget(mMessageWidget, "message");
        getWidget(mButtonsWidget, "buttons");

        mMessageWidget->setSize(400, mMessageWidget->getHeight());
        mMessageWidget->setCaptionWithReplacing(message);

        MyGUI::IntSize textSize = mMessageWidget->getTextSize();

        MyGUI::IntSize gameWindowSize = MyGUI::RenderManager::getInstance().getViewSize();

        int biggestButtonWidth = 0;
        int buttonsWidth = 0;
        int buttonsHeight = 0;
        int buttonHeight = 0;
        MyGUI::IntCoord dummyCoord(0, 0, 0, 0);

        if (hacking == nullptr)
        {
            for (std::size_t index = 0; index < buttons.size(); ++index)
            {
                MyGUI::Button* button = mButtonsWidget->createWidget<MyGUI::Button>(
                    MyGUI::WidgetStyle::Child, std::string("MW_Button"), dummyCoord, MyGUI::Align::Default);
                button->setCaptionWithReplacing(buttons[index]);
                button->eventMouseButtonClick += MyGUI::newDelegate(this, &InteractiveMessageBox::mousePressed);
                mButtons.push_back(button);
                mButtonValues.push_back(static_cast<int>(index));

                if (buttonsWidth != 0)
                    buttonsWidth += buttonLeftPadding;
                int buttonWidth = button->getTextSize().width + 2 * buttonLabelLeftPadding;
                buttonsWidth += buttonWidth;
                buttonHeight = button->getTextSize().height + 2 * buttonLabelTopPadding;
                if (buttonsHeight != 0)
                    buttonsHeight += buttonTopPadding;
                buttonsHeight += buttonHeight;
                if (buttonWidth > biggestButtonWidth)
                    biggestButtonWidth = buttonWidth;
            }
        }

        if (Settings::gui().mControllerMenus)
        {
            mDisableGamepadCursor = true;
            mControllerButtons.mA = "#{Interface:OK}";

            // If we have more than one button, we need to set the focus to the first one.
            if (mButtons.size() > 1)
            {
                mControllerFocus = 0;
                if (mDefaultFocus < mButtons.size())
                    mControllerFocus = mDefaultFocus;
                for (size_t i = 0; i < mButtons.size(); ++i)
                    mButtons[i]->setStateSelected(i == mControllerFocus);
            }
        }

        MyGUI::IntSize mainWidgetSize;
        if (hacking != nullptr)
        {
            if (fnvMenu == nullptr || hacking->mRows.size() != 56)
            {
                Log(Debug::Error) << "FNV hacking menu: rejected incomplete authored presentation";
                mMarkedToDelete = true;
                return;
            }
            const FnvMenuLayoutEvaluationContext layoutContext{
                static_cast<float>(gameWindowSize.width), static_cast<float>(gameWindowSize.height),
                [](std::string_view symbol) -> std::optional<float> {
                    if (symbol == "true" || symbol == "highdef" || symbol == "scale")
                        return 1.f;
                    if (symbol == "false" || symbol == "xbox")
                        return 0.f;
                    return std::nullopt;
                },
            };
            const float authoredWidth
                = evaluateFnvMenuNamedScalarTrait(fnvMenu->mRoot, frameTile, "width", layoutContext).value_or(920.f);
            const float authoredHeight
                = evaluateFnvMenuNamedScalarTrait(fnvMenu->mRoot, frameTile, "height", layoutContext).value_or(630.f);
            const float fit = std::min({ 1.f, gameWindowSize.width * 0.95f / authoredWidth,
                gameWindowSize.height * 0.95f / authoredHeight });
            const auto scaled = [fit](float value) { return static_cast<int>(value * fit); };
            const MyGUI::IntSize size(scaled(authoredWidth), scaled(authoredHeight));
            mMainWidget->setSize(size);
            mMainWidget->setPosition(
                (gameWindowSize.width - size.width) / 2, (gameWindowSize.height - size.height) / 2);
            mMessageWidget->setCoord(0, 0, size.width, scaled(150.f));
            mMessageWidget->setTextColour(MyGUI::Colour(0.2f, 1.f, 0.2f));
            mMessageWidget->setFontHeight(std::max(1, scaled(17.f)));

            const auto makeText = [&](int left, int top, int width, int height, const std::string& caption) {
                MyGUI::EditBox* text = mButtonsWidget->createWidget<MyGUI::EditBox>(MyGUI::WidgetStyle::Child,
                    std::string{}, MyGUI::IntCoord(left, top, width, height), MyGUI::Align::Default);
                text->setFontName("MonoFont");
                text->setFontHeight(std::max(1, scaled(17.f)));
                text->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
                text->setEditStatic(true);
                text->setEditMultiLine(true);
                text->setNeedMouseFocus(false);
                text->setTextColour(MyGUI::Colour(0.2f, 1.f, 0.2f));
                text->setCaption(caption);
            };
            const auto joinRows = [&](std::size_t begin) {
                std::string result;
                for (std::size_t row = 0; row < 28; ++row)
                {
                    if (row != 0)
                        result.push_back('\n');
                    result += hacking->mRows[begin + row];
                }
                return result;
            };
            const int fileY = scaled(150.f);
            makeText(0, fileY, scaled(323.f), scaled(476.f), joinRows(0));
            makeText(scaled(342.f), fileY, scaled(323.f), scaled(476.f), joinRows(28));
            makeText(scaled(665.f), fileY, scaled(255.f), scaled(476.f), hacking->mLog);

            constexpr std::size_t columnCapacity = 28 * 12;
            for (std::size_t targetIndex = 0; targetIndex < hacking->mTargets.size(); ++targetIndex)
            {
                std::size_t begin = hacking->mTargets[targetIndex].mBegin;
                const std::size_t end = hacking->mTargets[targetIndex].mEnd;
                while (begin <= end)
                {
                    const std::size_t column = begin / columnCapacity;
                    const std::size_t inColumn = begin % columnCapacity;
                    const std::size_t row = inColumn / 12;
                    const std::size_t character = inColumn % 12;
                    const std::size_t segmentEnd = std::min(end, begin + (12 - character) - 1);
                    MyGUI::Button* button = mButtonsWidget->createWidget<MyGUI::Button>(MyGUI::WidgetStyle::Child,
                        std::string{}, MyGUI::IntCoord(scaled((column == 0 ? 0.f : 342.f) + (7 + character) * 17.f),
                                           fileY + scaled(row * 17.f), scaled((segmentEnd - begin + 1) * 17.f),
                                           std::max(1, scaled(17.f))),
                        MyGUI::Align::Default);
                    button->eventMouseButtonClick += MyGUI::newDelegate(this, &InteractiveMessageBox::mousePressed);
                    button->eventMouseMove += MyGUI::newDelegate(this, &InteractiveMessageBox::mouseMoved);
                    mButtons.push_back(button);
                    mButtonValues.push_back(static_cast<int>(targetIndex));
                    if (mHackingControllerButtons.empty()
                        || mButtonValues[mHackingControllerButtons.back()] != static_cast<int>(targetIndex))
                        mHackingControllerButtons.push_back(mButtons.size() - 1);

                    MyGUI::EditBox* label = mButtonsWidget->createWidget<MyGUI::EditBox>(MyGUI::WidgetStyle::Child,
                        std::string{}, button->getCoord(), MyGUI::Align::Default);
                    label->setFontName("MonoFont");
                    label->setFontHeight(std::max(1, scaled(17.f)));
                    label->setEditStatic(true);
                    label->setNeedMouseFocus(false);
                    label->setTextColour(MyGUI::Colour(0.2f, 1.f, 0.2f));
                    label->setCaption(hacking->mRows[column * 28 + row].substr(
                        7 + character, segmentEnd - begin + 1));
                    mHackingLabels.push_back(label);
                    begin = segmentEnd + 1;
                }
            }

            if (Settings::gui().mControllerMenus && !mHackingControllerButtons.empty())
            {
                mDisableGamepadCursor = true;
                mControllerButtons.mA = "#{Interface:OK}";
                mControllerButtons.mB = "#{Interface:Back}";
                mControllerFocus = 0;
                setHackingFocus(mHackingControllerButtons.front());
            }

            MyGUI::ImageBox* background = nullptr;
            getWidget(background, "background");
            // MyGUI uses lower child depths for the foreground. Keep the authored CRT image behind the terminal
            // glyphs and hit targets; equal-depth layout order can otherwise cover the complete hacking surface.
            background->setDepth(100);
            background->setNeedMouseFocus(false);
            const FnvMenuXmlNode* image = fnvMenu->mRoot.findDescendantByName("hacking_background");
            if (image != nullptr && image->findChild("filename") != nullptr)
            {
                background->setImageTexture(normalizeFnvMenuTexturePath(image->findChild("filename")->mText));
            }
            setVisible(true);
            return;
        }
        else if (buttonsWidth < textSize.width)
        {
            // on one line
            mainWidgetSize.width = textSize.width + 3 * textPadding;
            mainWidgetSize.height
                = textPadding + textSize.height + textButtonPadding + buttonHeight + buttonMainPadding;

            MyGUI::IntSize realSize = mainWidgetSize +
                // To account for borders
                (mMainWidget->getSize() - mMainWidget->getClientWidget()->getSize());

            MyGUI::IntPoint absPos;
            absPos.left = (gameWindowSize.width - realSize.width) / 2;
            absPos.top = (gameWindowSize.height - realSize.height) / 2;

            mMainWidget->setPosition(absPos);
            mMainWidget->setSize(realSize);

            MyGUI::IntCoord messageWidgetCoord;
            messageWidgetCoord.left = (mainWidgetSize.width - textSize.width) / 2;
            messageWidgetCoord.top = textPadding;
            mMessageWidget->setCoord(messageWidgetCoord);

            mMessageWidget->setSize(textSize);

            MyGUI::IntCoord buttonCord;
            MyGUI::IntSize buttonSize(0, buttonHeight);
            int left = (mainWidgetSize.width - buttonsWidth) / 2;

            for (MyGUI::Button* button : mButtons)
            {
                buttonCord.left = left;
                buttonCord.top = messageWidgetCoord.top + textSize.height + textButtonPadding;

                buttonSize.width = button->getTextSize().width + 2 * buttonLabelLeftPadding;
                buttonSize.height = button->getTextSize().height + 2 * buttonLabelTopPadding;

                button->setCoord(buttonCord);
                button->setSize(buttonSize);

                left += buttonSize.width + buttonLeftPadding;
            }
        }
        else
        {
            // among each other
            if (biggestButtonWidth > textSize.width)
            {
                mainWidgetSize.width = biggestButtonWidth + buttonTopPadding * 2;
            }
            else
            {
                mainWidgetSize.width = textSize.width + 3 * textPadding;
            }

            MyGUI::IntCoord buttonCord;
            MyGUI::IntSize buttonSize(0, buttonHeight);

            int top = textPadding + textSize.height + textButtonPadding;

            for (MyGUI::Button* button : mButtons)
            {
                buttonSize.width = button->getTextSize().width + buttonLabelLeftPadding * 2;
                buttonSize.height = button->getTextSize().height + buttonLabelTopPadding * 2;

                buttonCord.top = top;
                buttonCord.left = (mainWidgetSize.width - buttonSize.width) / 2;

                button->setCoord(buttonCord);
                button->setSize(buttonSize);

                top += buttonSize.height + buttonTopPadding;
            }

            mainWidgetSize.height
                = textPadding + textSize.height + textButtonPadding + buttonsHeight + buttonMainPadding;
            mMainWidget->setSize(mainWidgetSize +
                // To account for borders
                (mMainWidget->getSize() - mMainWidget->getClientWidget()->getSize()));

            MyGUI::IntPoint absPos;
            absPos.left = (gameWindowSize.width - mainWidgetSize.width) / 2;
            absPos.top = (gameWindowSize.height - mainWidgetSize.height) / 2;

            mMainWidget->setPosition(absPos);

            MyGUI::IntCoord messageWidgetCoord;
            messageWidgetCoord.left = (mainWidgetSize.width - textSize.width) / 2;
            messageWidgetCoord.top = textPadding;
            messageWidgetCoord.width = textSize.width;
            messageWidgetCoord.height = textSize.height;
            mMessageWidget->setCoord(messageWidgetCoord);
        }

        if (fnvMenu != nullptr)
        {
            MyGUI::ImageBox* background = nullptr;
            getWidget(background, "background");
            background->setDepth(100);
            background->setNeedMouseFocus(false);
            const FnvMenuLayoutEvaluationContext layoutContext{
                static_cast<float>(gameWindowSize.width),
                static_cast<float>(gameWindowSize.height),
                [](std::string_view symbol) -> std::optional<float> {
                    if (symbol == "true" || symbol == "highdef" || symbol == "scale")
                        return 1.f;
                    if (symbol == "false" || symbol == "xbox")
                        return 0.f;
                    return std::nullopt;
                },
            };
            const std::optional<float> authoredWidth
                = evaluateFnvMenuNamedScalarTrait(fnvMenu->mRoot, frameTile, "width", layoutContext);
            const std::optional<float> authoredHeight
                = evaluateFnvMenuNamedScalarTrait(fnvMenu->mRoot, frameTile, "height", layoutContext);
            if (authoredWidth && authoredHeight && *authoredWidth > 0.f && *authoredHeight > 0.f)
            {
                const float fit = std::min({ 1.f, gameWindowSize.width * 0.95f / *authoredWidth,
                    gameWindowSize.height * 0.95f / *authoredHeight });
                const MyGUI::IntSize size(
                    static_cast<int>(*authoredWidth * fit), static_cast<int>(*authoredHeight * fit));
                mMainWidget->setSize(size);
                mMainWidget->setPosition(
                    (gameWindowSize.width - size.width) / 2, (gameWindowSize.height - size.height) / 2);

                const auto scaledTrait = [&](std::string_view node, std::string_view trait,
                                             float fallback) -> int {
                    return static_cast<int>(evaluateFnvMenuNamedScalarTrait(fnvMenu->mRoot, node, trait, layoutContext)
                                                .value_or(fallback)
                        * fit);
                };
                const int messageX = scaledTrait(messageTile, "x", 48.f);
                const int messageY = scaledTrait(messageTile, "y", 40.f);
                const int buttonX = scaledTrait(buttonTile, "x", 48.f);
                const int buttonY = scaledTrait(buttonTile, "y", 180.f);
                const int buttonWidth = scaledTrait(buttonTile, "width", *authoredWidth - 96.f);
                mMessageWidget->setCoord(messageX, messageY,
                    std::max(1, size.width - messageX * 2), std::max(1, buttonY - messageY - 8));
                int top = buttonY;
                for (MyGUI::Button* button : mButtons)
                {
                    const int height = std::max(24, static_cast<int>(button->getTextSize().height * 1.35f));
                    button->setCoord(buttonX, top, std::max(1, buttonWidth), height);
                    button->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                    button->setTextColour(MyGUI::Colour(0.2f, 1.f, 0.2f));
                    top += height + 4;
                }
            }
            mMessageWidget->setTextColour(MyGUI::Colour(0.2f, 1.f, 0.2f));
            const auto findBackground = [](const auto& self, const FnvMenuXmlNode& node) -> const FnvMenuXmlNode* {
                if (node.mType == "image" && node.findChild("filename") != nullptr)
                    return &node;
                for (const FnvMenuXmlNode& child : node.mChildren)
                {
                    if (const FnvMenuXmlNode* result = self(self, child))
                        return result;
                }
                return nullptr;
            };
            if (const FnvMenuXmlNode* image = findBackground(findBackground, fnvMenu->mRoot))
            {
                background->setImageTexture(normalizeFnvMenuTexturePath(image->findChild("filename")->mText));
            }
        }

        setVisible(true);
    }

    MyGUI::Widget* InteractiveMessageBox::getDefaultKeyFocus()
    {
        if (mDefaultFocus < mButtons.size())
            return mButtons[mDefaultFocus];
        auto& languageManager = MyGUI::LanguageManager::getInstance();
        std::vector<MyGUI::UString> keywords{ languageManager.replaceTags("#{sOk}"),
            languageManager.replaceTags("#{sYes}") };

        for (MyGUI::Button* button : mButtons)
        {
            for (const MyGUI::UString& keyword : keywords)
            {
                if (Misc::StringUtils::ciEqual(keyword, button->getCaption()))
                {
                    return button;
                }
            }
        }
        return nullptr;
    }

    void InteractiveMessageBox::closeDefault() 
    {
        if (mButtons.empty())
            return;
        auto buttonIndex = std::min(mDefaultFocus, mButtons.size() - 1);
        auto button = mButtons[buttonIndex];
        mousePressed(button);
    }

    void InteractiveMessageBox::mousePressed(MyGUI::Widget* widget)
    {
        buttonActivated(widget);
    }

    void InteractiveMessageBox::mouseMoved(MyGUI::Widget* widget, int, int)
    {
        const auto found = std::find(mButtons.begin(), mButtons.end(), widget);
        if (found != mButtons.end())
            setHackingFocus(static_cast<std::size_t>(std::distance(mButtons.begin(), found)));
    }

    void InteractiveMessageBox::setHackingFocus(std::size_t buttonIndex)
    {
        if (!mHacking || buttonIndex >= mButtonValues.size())
            return;
        const int target = mButtonValues[buttonIndex];
        for (std::size_t index = 0; index < mHackingLabels.size(); ++index)
            mHackingLabels[index]->setTextColour(mButtonValues[index] == target ? MyGUI::Colour::White
                                                                                : MyGUI::Colour(0.2f, 1.f, 0.2f));
    }

    void InteractiveMessageBox::finish(int result)
    {
        mMarkedToDelete = true;
        mButtonPressed = result;
        mMessageBoxManager.onButtonPressed(result);
        if (!mImmediate)
            return;
        mMessageBoxManager.setLastButtonPressed(result);
        MWBase::Environment::get().getInputManager()->changeInputMode(
            MWBase::Environment::get().getWindowManager()->isGuiMode());
    }

    bool InteractiveMessageBox::exit()
    {
        if (!mHacking)
            return false;
        finish(-2);
        return true;
    }

    void InteractiveMessageBox::buttonActivated(MyGUI::Widget* widget)
    {
        mMarkedToDelete = true;
        int index = 0;
        for (const MyGUI::Button* button : mButtons)
        {
            if (button == widget)
            {
                const int result = static_cast<std::size_t>(index) < mButtonValues.size()
                    ? mButtonValues[static_cast<std::size_t>(index)]
                    : index;
                finish(result);
                return;
            }
            index++;
        }
    }

    int InteractiveMessageBox::readPressedButton()
    {
        return mButtonPressed;
    }

    bool InteractiveMessageBox::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mHacking && !mHackingControllerButtons.empty())
            {
                if (mControllerFocus >= mHackingControllerButtons.size())
                    mControllerFocus = mHackingControllerButtons.size() - 1;
                buttonActivated(mButtons[mHackingControllerButtons[mControllerFocus]]);
            }
            else if (!mButtons.empty())
            {
                if (mControllerFocus >= mButtons.size())
                    mControllerFocus = mButtons.size() - 1;
                buttonActivated(mButtons[mControllerFocus]);
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            if (mHacking)
                exit();
            else if (mButtons.size() == 1)
                buttonActivated(mButtons[0]);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP || arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        {
            if (mHacking)
            {
                if (mHackingControllerButtons.size() <= 1)
                    return true;
                mControllerFocus = wrap(mControllerFocus - 1, mHackingControllerButtons.size());
                setHackingFocus(mHackingControllerButtons[mControllerFocus]);
                return true;
            }
            if (mButtons.size() <= 1)
                return true;
            if (mButtons.size() == 2 && mControllerFocus == 0)
                return true;

            setControllerFocus(mButtons, mControllerFocus, false);
            mControllerFocus = wrap(mControllerFocus - 1, mButtons.size());
            setControllerFocus(mButtons, mControllerFocus, true);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN || arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            if (mHacking)
            {
                if (mHackingControllerButtons.size() <= 1)
                    return true;
                mControllerFocus = wrap(mControllerFocus + 1, mHackingControllerButtons.size());
                setHackingFocus(mHackingControllerButtons[mControllerFocus]);
                return true;
            }
            if (mButtons.size() <= 1)
                return true;
            if (mButtons.size() == 2 && mControllerFocus == 1)
                return true;

            setControllerFocus(mButtons, mControllerFocus, false);
            mControllerFocus = wrap(mControllerFocus + 1, mButtons.size());
            setControllerFocus(mButtons, mControllerFocus, true);
        }

        return true;
    }
}
