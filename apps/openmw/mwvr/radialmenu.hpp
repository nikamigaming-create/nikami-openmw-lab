#ifndef OPENMW_GAME_MWVR_RADIALMENU_H
#define OPENMW_GAME_MWVR_RADIALMENU_H

#include "../mwgui/windowbase.hpp"
#include "components/widgets/box.hpp"
#include <MyGUI_Button.h>

#include <osg/MatrixTransform>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

#include <array>
#include <vector>

#include "../mwworld/ptr.hpp"

namespace Gui
{
    class ImageButton;
}

namespace VFS
{
    class Manager;
}

namespace Resource
{
    class ResourceSystem;
}

namespace osg
{
    class Group;
}

namespace MWGui
{
    class QuickKeysMenu;
}

namespace MWVR
{
    class RadialMenu : public MWGui::WindowBase
    {
        int mWidth;
        int mHeight;
        MWGui::QuickKeysMenu* mQkm;
        Resource::ResourceSystem* mResourceSystem;
        osg::observer_ptr<osg::Group> mSceneRoot;
        osg::ref_ptr<osg::Group> mWeaponWheelRoot;

        struct WheelWeapon
        {
            MWWorld::Ptr mItem;
            osg::ref_ptr<osg::MatrixTransform> mTransform;
            osg::Vec3f mModelCenter;
            osg::Vec3f mWorldCenter;
            float mBaseScale = 1.f;
        };

        std::vector<WheelWeapon> mWheelWeapons;
        int mHoveredWeapon = -1;
        bool mWheelVisible = false;
        bool mRadialWasDown = false;
        bool mGripWasDown = false;
        bool mWheelTelemetryLogged = false;
        bool mWheelBasisValid = false;
        osg::Vec3f mWheelNormal;
        osg::Vec3f mWheelRight;
        osg::Vec3f mWheelUp;
        float mOpenAmount = 0.f;

    public:
        RadialMenu(int w, int h, MWGui::QuickKeysMenu* qkm, osg::Group* sceneRoot,
            Resource::ResourceSystem* resourceSystem);
        ~RadialMenu();

        void onResChange(int w, int h) override;

        void setVisible(bool visible) override;

        void onFrame(float dt) override;

        bool exit() override;

    private:
        void onButtonClicked(MyGUI::Widget* sender);
        void close();

        void initMenu();
        void updateMenu();
        void rebuildWeaponWheel();
        void updateWeaponWheel(float dt);
        void selectWheelWeapon(int index, const char* interaction);
        void clearWeaponWheel();
    };

}

#endif
