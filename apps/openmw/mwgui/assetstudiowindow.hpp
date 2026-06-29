#ifndef OPENMW_MWGUI_ASSETSTUDIOWINDOW_H
#define OPENMW_MWGUI_ASSETSTUDIOWINDOW_H

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <osg/Vec3f>

#include "../mwrender/characterpreview.hpp"

#include "windowbase.hpp"

namespace MyGUI
{
    class Button;
    class ComboBox;
    class EditBox;
    class ImageBox;
    class ListBox;
    class ScrollBar;
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
        void onFrame(float duration) override;

    private:
        void onApply(MyGUI::Widget* sender);
        void onSave(MyGUI::Widget* sender);
        void onSnap(MyGUI::Widget* sender);
        void onViewButton(MyGUI::Widget* sender);
        void onProfileButton(MyGUI::Widget* sender);
        void onDebugButton(MyGUI::Widget* sender);
        void onAssetCategoryChanged(MyGUI::ComboBox* sender, size_t index);
        void onAssetCategoryListChanged(MyGUI::ListBox* sender, size_t index);
        void onAssetSearchChanged(MyGUI::EditBox* sender);
        void onAssetSelected(MyGUI::ListBox* sender, size_t index);
        void onAssetAccepted(MyGUI::ListBox* sender, size_t index);
        void onAssetLoad(MyGUI::Widget* sender);
        void onScreenshot(MyGUI::Widget* sender);
        void onAccept(MyGUI::EditBox* sender);
        void onSliderMoved(MyGUI::ScrollBar* sender, size_t position);
        void applyStudioCommand(const std::string& command, const std::string& value = {});
        void rebuildPreview();
        void rebuildMainPreview();
        void pollCommandFile(float duration);
        bool applyCommandFilePayload(const std::string& content);
        bool setStudioField(const std::string& field, const std::string& value);
        bool rebuildActorPreview(const std::string& assetClass, const std::string& record, const std::string& session,
            const std::string& view, const std::string& profile, float zoom, bool rebuildCameraTiles);
        bool rebuildModelPreview(const std::string& assetClass, const std::string& record, const std::string& session,
            const std::string& view, const std::string& model, float zoom, bool rebuildCameraTiles);
        std::string resolveRecordModel(const std::string& assetClass, const std::string& record) const;
        osg::Vec3f cameraDirectionForView(const std::string& view) const;
        MWRender::FalloutActorPreview::ViewMode actorViewModeForView(const std::string& view) const;
        void attachTexture(MyGUI::ImageBox* image, MyGUIPlatform::OSGTexture* texture) const;
        float editFloat(MyGUI::EditBox* edit, float fallback) const;
        std::string editText(MyGUI::EditBox* edit) const;
        void configureSliders();
        void syncSlidersFromEdits();
        void setSliderFromFloat(MyGUI::ScrollBar* slider, float value, float minValue, float maxValue, float step);
        float sliderFloat(MyGUI::ScrollBar* slider, float minValue, float step) const;
        void setEditFloat(MyGUI::EditBox* edit, float value);
        void saveSession();
        bool isActorAssetClass(const std::string& assetClass) const;
        void populateAssetCategories();
        void updateAssetResults();
        void applySelectedAsset(bool rebuild);
        void requestScreenshot();
        void retireCurrentPreviews();
        void clearRetiredPreviews();
        void clearPendingPreviews();
        void promotePendingPreviews();
        std::string selectedAssetClass() const;
        void setStatus(const std::string& value);
        void updateToggleCaptions();
        std::string cameraTileView(std::size_t index) const;
        bool promoteCameraTile(const std::string& view);

        osg::Group* mParent = nullptr;
        Resource::ResourceSystem* mResourceSystem = nullptr;

        MyGUI::TextBox* mTitle = nullptr;
        MyGUI::ImageBox* mPreviewImage = nullptr;
        MyGUI::EditBox* mAssetClassEdit = nullptr;
        MyGUI::EditBox* mRecordEdit = nullptr;
        MyGUI::EditBox* mSessionEdit = nullptr;
        MyGUI::EditBox* mModelEdit = nullptr;
        MyGUI::ComboBox* mAssetCategoryCombo = nullptr;
        MyGUI::ListBox* mAssetCategoryList = nullptr;
        MyGUI::EditBox* mAssetSearchEdit = nullptr;
        MyGUI::ListBox* mAssetResultList = nullptr;
        MyGUI::TextBox* mAssetCountText = nullptr;
        MyGUI::Button* mAssetLoadButton = nullptr;
        MyGUI::Button* mScreenshotButton = nullptr;
        MyGUI::EditBox* mViewEdit = nullptr;
        MyGUI::EditBox* mProfileEdit = nullptr;
        MyGUI::EditBox* mRotXEdit = nullptr;
        MyGUI::EditBox* mRotYEdit = nullptr;
        MyGUI::EditBox* mRotZEdit = nullptr;
        MyGUI::EditBox* mScaleEdit = nullptr;
        MyGUI::EditBox* mZoomEdit = nullptr;
        MyGUI::EditBox* mPanXEdit = nullptr;
        MyGUI::EditBox* mPanZEdit = nullptr;
        MyGUI::EditBox* mTiltEdit = nullptr;
        MyGUI::ScrollBar* mRotXSlider = nullptr;
        MyGUI::ScrollBar* mRotYSlider = nullptr;
        MyGUI::ScrollBar* mRotZSlider = nullptr;
        MyGUI::ScrollBar* mScaleSlider = nullptr;
        MyGUI::ScrollBar* mZoomSlider = nullptr;
        MyGUI::Button* mApplyButton = nullptr;
        MyGUI::Button* mSaveButton = nullptr;
        MyGUI::Button* mSnapXNegButton = nullptr;
        MyGUI::Button* mSnapXZeroButton = nullptr;
        MyGUI::Button* mSnapXPosButton = nullptr;
        MyGUI::Button* mSnapYNegButton = nullptr;
        MyGUI::Button* mSnapYPosButton = nullptr;
        MyGUI::Button* mSnapZPosButton = nullptr;
        MyGUI::Button* mViewFrontButton = nullptr;
        MyGUI::Button* mViewLeftButton = nullptr;
        MyGUI::Button* mViewTopButton = nullptr;
        MyGUI::Button* mProfileFaceButton = nullptr;
        MyGUI::Button* mProfileBodyButton = nullptr;
        MyGUI::Button* mProfileHandsButton = nullptr;
        MyGUI::Button* mProfileWeaponButton = nullptr;
        MyGUI::Button* mAxesButton = nullptr;
        MyGUI::Button* mIkButton = nullptr;
        MyGUI::Button* mWireframeButton = nullptr;
        MyGUI::Button* mWeightsButton = nullptr;
        MyGUI::Button* mBloodButton = nullptr;
        MyGUI::Button* mInventoryButton = nullptr;
        MyGUI::Button* mEquipmentButton = nullptr;
        std::array<MyGUI::Widget*, 8> mCameraHotspots{};
        MyGUI::TextBox* mStatus = nullptr;
        std::array<MyGUI::ImageBox*, 8> mCameraImages{};
        bool mSyncingSliders = false;
        bool mAxesEnabled = false;
        bool mIkEnabled = false;
        bool mWireframeEnabled = false;
        bool mWeightsEnabled = false;
        bool mBloodVisible = true;
        float mCommandPollElapsed = 0.f;
        std::string mLastCommandFileContent;
        std::string mLastCommandSequence;
        bool mUpdatingAssetResults = false;
        std::vector<std::string> mAssetCategoryClasses;
        std::string mSelectedAssetCategory = "all";
        float mRetiredPreviewSeconds = 0.f;
        float mPendingPreviewSeconds = 0.f;
        int mPendingPreviewFrames = 0;
        bool mPendingCameraTiles = false;

        std::unique_ptr<MWRender::StandaloneModelPreview> mPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mPreviewTexture;
        std::array<std::unique_ptr<MWRender::StandaloneModelPreview>, 8> mCameraPreviews;
        std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 8> mCameraPreviewTextures;
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Npc>> mActorNpcProxy;
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Creature>> mActorCreatureProxy;
        std::unique_ptr<MWRender::FalloutActorPreview> mActorPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mActorPreviewTexture;
        std::array<std::unique_ptr<MWRender::FalloutActorPreview>, 8> mActorCameraPreviews;
        std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 8> mActorCameraPreviewTextures;

        std::unique_ptr<MWRender::StandaloneModelPreview> mRetiredPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mRetiredPreviewTexture;
        std::array<std::unique_ptr<MWRender::StandaloneModelPreview>, 8> mRetiredCameraPreviews;
        std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 8> mRetiredCameraPreviewTextures;
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Npc>> mRetiredActorNpcProxy;
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Creature>> mRetiredActorCreatureProxy;
        std::unique_ptr<MWRender::FalloutActorPreview> mRetiredActorPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mRetiredActorPreviewTexture;
        std::array<std::unique_ptr<MWRender::FalloutActorPreview>, 8> mRetiredActorCameraPreviews;
        std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 8> mRetiredActorCameraPreviewTextures;

        std::unique_ptr<MWRender::StandaloneModelPreview> mPendingPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mPendingPreviewTexture;
        std::array<std::unique_ptr<MWRender::StandaloneModelPreview>, 8> mPendingCameraPreviews;
        std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 8> mPendingCameraPreviewTextures;
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Npc>> mPendingActorNpcProxy;
        std::unique_ptr<MWWorld::LiveCellRef<ESM4::Creature>> mPendingActorCreatureProxy;
        std::unique_ptr<MWRender::FalloutActorPreview> mPendingActorPreview;
        std::unique_ptr<MyGUIPlatform::OSGTexture> mPendingActorPreviewTexture;
        std::array<std::unique_ptr<MWRender::FalloutActorPreview>, 8> mPendingActorCameraPreviews;
        std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 8> mPendingActorCameraPreviewTextures;
    };
}

#endif
