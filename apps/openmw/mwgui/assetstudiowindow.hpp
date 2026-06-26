#ifndef OPENMW_MWGUI_ASSETSTUDIOWINDOW_H
#define OPENMW_MWGUI_ASSETSTUDIOWINDOW_H

#include <memory>
#include <string>

#include "windowbase.hpp"

namespace MyGUI
{
    class Button;
    class EditBox;
    class ImageBox;
    class TextBox;
    class Widget;
}

namespace MyGUIPlatform
{
    class OSGTexture;
}

namespace MWRender
{
    class StandaloneModelPreview;
}

namespace osg
{
    class Group;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWGui
{
    class AssetStudioWindow : public WindowBase
    {
    public:
        AssetStudioWindow(osg::Group* parent, Resource::ResourceSystem* resourceSystem);
        ~AssetStudioWindow() override;

    private:
        void onApply(MyGUI::Widget* sender);
        void onAccept(MyGUI::EditBox* sender);
        void rebuildPreview();
        float editFloat(MyGUI::EditBox* edit, float fallback) const;
        std::string editText(MyGUI::EditBox* edit) const;
        void setStatus(const std::string& value);

        osg::Group* mParent = nullptr;
        Resource::ResourceSystem* mResourceSystem = nullptr;

        MyGUI::ImageBox* mPreviewImage = nullptr;
        MyGUI::EditBox* mModelEdit = nullptr;
        MyGUI::EditBox* mRotXEdit = nullptr;
        MyGUI::EditBox* mRotYEdit = nullptr;
        MyGUI::EditBox* mRotZEdit = nullptr;
        MyGUI::EditBox* mScaleEdit = nullptr;
        MyGUI::Button* mApplyButton = nullptr;
        MyGUI::TextBox* mStatus = nullptr;

        std::unique_ptr<MWRender::StandaloneModelPreview> mPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mPreviewTexture;
    };
}

#endif
