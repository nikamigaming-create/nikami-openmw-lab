#ifndef MWGUI_INVENTORYTABSSOVERLAY_H
#define MWGUI_INVENTORYTABSSOVERLAY_H

#include "windowbase.hpp"

namespace MyGUI
{
    class Button;
    struct MouseButton;
}

namespace MWGui
{
    class InventoryTabsOverlay : public WindowBase
    {
    public:
        InventoryTabsOverlay();

        int getHeight();
        void setTab(int index);

    private:
        std::vector<MyGUI::Button*> mTabs;

        void onTabPressed(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton button);
    };
}

#endif
