#ifndef OPENMW_MWGUI_ASSETSTUDIOWINDOW_H
#define OPENMW_MWGUI_ASSETSTUDIOWINDOW_H

#include <array>
#include <memory>
#include <string>

#include <osg/Vec3f>

#include "../mwrender/characterpreview.hpp"

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

namespace ESM4
{
    struct Creature;
    struct Npc;
}

namespace MWWorld
{
    template <typename X>
    struct LiveCellRef;
    class Ptr;
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
        void onResChange(int width, int height) override;

    private:
        void onApply(MyGUI::Widget* sender);
        void onAccept(MyGUI::EditBox* sender);
        void rebuildPreview();
        bool rebuildActorPreview(const std::string& assetClass, const std::string& record, const std::string& session,
            const std::string& view, float zoom);
        bool rebuildModelPreview(const std::string& assetClass, const std::string& record, const std::string& session,
            const std::string& view, const std::string& model, float zoom);
        osg::Vec3f cameraDirectionForView(const std::string& view) const;
        MWRender::FalloutActorPreview::ViewMode actorViewModeForView(const std::string& view) const;
        void attachTexture(MyGUI::ImageBox* image, MyGUIPlatform::OSGTexture* texture) const;
        float editFloat(MyGUI::EditBox* edit, float fallback) const;
        std::string editText(MyGUI::EditBox* edit) const;
        bool isActorAssetClass(const std::string& assetClass) const;
        void setStatus(const std::string& value);

        osg::Group* mParent = nullptr;
        Resource::ResourceSystem* mResourceSystem = nullptr;

        MyGUI::TextBox* mTitle = nullptr;
        MyGUI::ImageBox* mPreviewImage = nullptr;
        MyGUI::EditBox* mAssetClassEdit = nullptr;
        MyGUI::EditBox* mRecordEdit = nullptr;
        MyGUI::EditBox* mSessionEdit = nullptr;
        MyGUI::EditBox* mModelEdit = nullptr;
        MyGUI::EditBox* mViewEdit = nullptr;
        MyGUI::EditBox* mRotXEdit = nullptr;
        MyGUI::EditBox* mRotYEdit = nullptr;
        MyGUI::EditBox* mRotZEdit = nullptr;
        MyGUI::EditBox* mScaleEdit = nullptr;
        MyGUI::EditBox* mZoomEdit = nullptr;
        MyGUI::Button* mApplyButton = nullptr;
        MyGUI::TextBox* mStatus = nullptr;
        std::array<MyGUI::ImageBox*, 3> mCameraImages{};

        std::unique_ptr<MWRender::StandaloneModelPreview> mPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mPreviewTexture;
        std::array<std::unique_ptr<MWRender::StandaloneModelPreview>, 3> mCameraPreviews;
        std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 3> mCameraPreviewTextures;
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Npc>> mActorNpcProxy;
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Creature>> mActorCreatureProxy;
        std::unique_ptr<MWRender::FalloutActorPreview> mActorPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mActorPreviewTexture;
        std::array<std::unique_ptr<MWRender::FalloutActorPreview>, 3> mActorCameraPreviews;
        std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 3> mActorCameraPreviewTextures;
    };
}

#endif
