#include "assetstudiowindow.hpp"

#include <cstdlib>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Widget.h>
#include <MyGUI_Window.h>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/myguitexture.hpp>

#include "../mwrender/standalonemodelpreview.hpp"

namespace MWGui
{
    namespace
    {
        std::string getenvText(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr ? std::string(value) : std::string();
        }
    }

    AssetStudioWindow::AssetStudioWindow(osg::Group* parent, Resource::ResourceSystem* resourceSystem)
        : WindowBase("openmw_asset_studio_window.layout")
        , mParent(parent)
        , mResourceSystem(resourceSystem)
    {
        getWidget(mPreviewImage, "PreviewImage");
        getWidget(mModelEdit, "ModelEdit");
        getWidget(mRotXEdit, "RotXEdit");
        getWidget(mRotYEdit, "RotYEdit");
        getWidget(mRotZEdit, "RotZEdit");
        getWidget(mScaleEdit, "ScaleEdit");
        getWidget(mApplyButton, "ApplyButton");
        getWidget(mStatus, "StatusText");

        mApplyButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onApply);
        mModelEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);

        const std::string initialModel = getenvText("OPENMW_FNV_ASSET_STUDIO_MODEL");
        if (!initialModel.empty())
            mModelEdit->setCaption(initialModel);

        mRotXEdit->setCaption(getenvText("OPENMW_FNV_ASSET_STUDIO_ROT_X").empty() ? "0" : getenvText("OPENMW_FNV_ASSET_STUDIO_ROT_X"));
        mRotYEdit->setCaption(getenvText("OPENMW_FNV_ASSET_STUDIO_ROT_Y").empty() ? "0" : getenvText("OPENMW_FNV_ASSET_STUDIO_ROT_Y"));
        mRotZEdit->setCaption(getenvText("OPENMW_FNV_ASSET_STUDIO_ROT_Z").empty() ? "0" : getenvText("OPENMW_FNV_ASSET_STUDIO_ROT_Z"));
        mScaleEdit->setCaption(getenvText("OPENMW_FNV_ASSET_STUDIO_SCALE").empty() ? "1" : getenvText("OPENMW_FNV_ASSET_STUDIO_SCALE"));

        mMainWidget->castType<MyGUI::Window>()->setCaption("FNV Asset Studio");
        setVisible(true);

        Log(Debug::Info) << "FNV/ESM4 asset studio native window opened model=\"" << initialModel
                         << "\" gate=native-asset-studio-window";
        rebuildPreview();
    }

    AssetStudioWindow::~AssetStudioWindow() = default;

    std::string AssetStudioWindow::editText(MyGUI::EditBox* edit) const
    {
        return edit != nullptr ? edit->getOnlyText().asUTF8() : std::string();
    }

    float AssetStudioWindow::editFloat(MyGUI::EditBox* edit, float fallback) const
    {
        const std::string text = editText(edit);
        if (text.empty())
            return fallback;

        char* end = nullptr;
        const float parsed = std::strtof(text.c_str(), &end);
        if (end == text.c_str())
            return fallback;
        return parsed;
    }

    void AssetStudioWindow::setStatus(const std::string& value)
    {
        if (mStatus != nullptr)
            mStatus->setCaption(value);
    }

    void AssetStudioWindow::onApply(MyGUI::Widget*)
    {
        rebuildPreview();
    }

    void AssetStudioWindow::onAccept(MyGUI::EditBox*)
    {
        rebuildPreview();
    }

    void AssetStudioWindow::rebuildPreview()
    {
        const std::string model = editText(mModelEdit);
        if (model.empty())
        {
            mPreview.reset();
            mPreviewTexture.reset();
            mPreviewImage->setRenderItemTexture(nullptr);
            setStatus("Enter a model path, then Apply.");
            Log(Debug::Info) << "FNV/ESM4 asset studio idle reason=no-model gate=native-asset-studio-window";
            return;
        }

        try
        {
            MWRender::StandaloneModelPreviewSettings settings;
            settings.mModel = model;
            settings.mRotation = osg::Vec3f(editFloat(mRotXEdit, 0.f), editFloat(mRotYEdit, 0.f), editFloat(mRotZEdit, 0.f));
            settings.mScale = editFloat(mScaleEdit, 1.f);
            settings.mWidth = 1280;
            settings.mHeight = 900;
            settings.mClearColor = osg::Vec4f(0.07f, 0.08f, 0.09f, 1.f);

            mPreview = std::make_unique<MWRender::StandaloneModelPreview>(mParent, mResourceSystem, settings);
            mPreviewTexture = std::make_unique<MyGUIPlatform::OSGTexture>(mPreview->getTexture());
            mPreviewImage->setRenderItemTexture(mPreviewTexture.get());
            mPreviewImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));

            const MWRender::StandaloneModelPreviewState& state = mPreview->getState();
            setStatus("loaded " + model + " bounds="
                + (state.mBoundsValid ? "valid" : "invalid"));
            Log(Debug::Info) << "FNV/ESM4 asset studio model loaded model=\"" << model
                             << "\" correctedModel=\"" << state.mCorrectedModel << "\" boundsValid="
                             << state.mBoundsValid << " boundsSize=(" << state.mBoundsSize.x() << ","
                             << state.mBoundsSize.y() << "," << state.mBoundsSize.z()
                             << ") gate=native-asset-studio-model-preview runtime=runtime-supported";
        }
        catch (const std::exception& e)
        {
            mPreview.reset();
            mPreviewTexture.reset();
            mPreviewImage->setRenderItemTexture(nullptr);
            setStatus(std::string("failed: ") + e.what());
            Log(Debug::Warning) << "FNV/ESM4 asset studio model failed model=\"" << model << "\" error=\""
                                << e.what() << "\" gate=native-asset-studio-model-preview runtime=known-blocked";
        }
    }
}
