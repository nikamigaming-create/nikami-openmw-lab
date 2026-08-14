#ifndef OPENMW_LUAUI_WINDOW
#define OPENMW_LUAUI_WINDOW

<<<<<<< HEAD
#include <vector>
=======
#include <optional>
>>>>>>> origin/main

#include "text.hpp"
#include "widget.hpp"

namespace LuaUi
{
    class LuaWindow : public MyGUI::Widget, public WidgetExtension
    {
        MYGUI_RTTI_DERIVED(LuaWindow)

    public:
        LuaWindow();
        void updateTemplate() override;
        void updateProperties() override;

    private:
        LuaText* mCaption;
        std::map<MyGUI::Widget*, WidgetExtension*> mActionWidgets;
        MyGUI::IntPoint mPreviousMouse;
        MyGUI::IntCoord mChangeScale;

        MyGUI::IntCoord mMoveResize;

<<<<<<< HEAD
        const std::vector<std::string_view>& allUsedProperties() const override;

=======
>>>>>>> origin/main
    protected:
        void notifyMousePress(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id);
        void notifyMouseDrag(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id);
    };
}

#endif // OPENMW_LUAUI_WINDOW
