#ifndef OPENMW_LUAUI_TEXTEDIT
#define OPENMW_LUAUI_TEXTEDIT

<<<<<<< HEAD
#include <vector>

=======
>>>>>>> origin/main
#include <MyGUI_EditBox.h>

#include "widget.hpp"

namespace LuaUi
{
    class LuaTextEdit : public MyGUI::Widget, public WidgetExtension
    {
        MYGUI_RTTI_DERIVED(LuaTextEdit)

    public:
        bool isTextInput() override { return mEditBox->getEditStatic(); }

    protected:
        void initialize() override;
        void deinitialize() override;
        void updateProperties() override;
        void updateCoord() override;
        void updateChildren() override;
        MyGUI::IntSize calculateSize() const override;

<<<<<<< HEAD
        const std::vector<std::string_view>& allUsedProperties() const override;

=======
>>>>>>> origin/main
    private:
        void textChange(MyGUI::EditBox*);

        MyGUI::EditBox* mEditBox = nullptr;
        bool mMultiline{ false };
        bool mAutoSize{ false };
    };
}

#endif // OPENMW_LUAUI_TEXTEDIT
