#include "inventorytabsoverlay.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_Widget.h>
#include <MyGUI_Window.h>

#include <components/debug/debuglog.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

namespace MWGui
{
    InventoryTabsOverlay::InventoryTabsOverlay()
        : WindowBase("openmw_inventory_tabs.layout")
    {
        MyGUI::Button* tab;
        static const char* kTabIds[] = { "TabMap", "TabInventory", "TabSpells", "TabStats" };

        for (const char* id : kTabIds)
        {
            getWidget(tab, id);
            // Switch on press. Waiting for MyGUI's synthetic click can lose the event when changing panes also
            // changes the window stack under the pointer.
            tab->eventMouseButtonPressed += MyGUI::newDelegate(this, &InventoryTabsOverlay::onTabPressed);
            mTabs.push_back(tab);
        }

        MyGUI::ImageBox* image;
        getWidget(image, "BtnL2Image");
        image->setImageTexture(
            MWBase::Environment::get().getInputManager()->getControllerAxisIcon(SDL_CONTROLLER_AXIS_TRIGGERLEFT));

        getWidget(image, "BtnR2Image");
        image->setImageTexture(
            MWBase::Environment::get().getInputManager()->getControllerAxisIcon(SDL_CONTROLLER_AXIS_TRIGGERRIGHT));

        // The overlay is only the height of the top strip. Keep its container focusable so MyGUI can
        // route presses to the child tab buttons; disabling focus on the parent also starves its children.
        mMainWidget->setNeedMouseFocus(true);
    }

    int InventoryTabsOverlay::getHeight()
    {
        MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
        return window->getHeight();
    }

    void InventoryTabsOverlay::onTabPressed(
        MyGUI::Widget* sender, int /*left*/, int /*top*/, MyGUI::MouseButton button)
    {
        if (button != MyGUI::MouseButton::Left)
            return;

        for (int i = 0; i < static_cast<int>(mTabs.size()); i++)
        {
            if (mTabs[i] == sender)
            {
                Log(Debug::Info) << "FNV/ESM4 input: Pip-Boy tab pressed index=" << i;
                MWBase::Environment::get().getWindowManager()->setActiveControllerWindow(GM_Inventory, i);
                setTab(i);
                break;
            }
        }
    }

    void InventoryTabsOverlay::setTab(int index)
    {
        for (int i = 0; i < static_cast<int>(mTabs.size()); i++)
            mTabs[i]->setStateSelected(i == index);
    }

}
