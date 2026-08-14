#ifndef MWGUI_INVENTORYTABSSOVERLAY_H
#define MWGUI_INVENTORYTABSSOVERLAY_H

#include "windowbase.hpp"

namespace MyGUI
{
    class Button;
<<<<<<< HEAD
=======
    struct MouseButton;
>>>>>>> origin/main
}

namespace MWGui
{
    class InventoryTabsOverlay : public WindowBase
    {
    public:
        InventoryTabsOverlay();

        int getHeight();
<<<<<<< HEAD
        void setTab(size_t index);
=======
        void setTab(int index);
>>>>>>> origin/main

    private:
        std::vector<MyGUI::Button*> mTabs;

<<<<<<< HEAD
        void onTabClicked(MyGUI::Widget* sender);
=======
        void onTabPressed(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton button);
>>>>>>> origin/main
    };
}

#endif
