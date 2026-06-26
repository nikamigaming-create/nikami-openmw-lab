#include "assetstudiowindow.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Widget.h>

#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/myguiplatform/myguitexture.hpp>

#include "../mwbase/environment.hpp"
#include "../mwrender/standalonemodelpreview.hpp"
#include "../mwworld/cellref.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/livecellref.hpp"
#include "../mwworld/ptr.hpp"

namespace MWGui
{
    namespace
    {
        std::string getenvText(const char* name)
        {
            const char* value = std::getenv(name);
            return value != nullptr ? std::string(value) : std::string();
        }

        std::string getenvTextOr(const char* name, std::string fallback)
        {
            std::string value = getenvText(name);
            return value.empty() ? fallback : value;
        }

        std::string lowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool targetMatches(std::string_view target, std::string_view candidate)
        {
            if (target.empty() || candidate.empty())
                return false;

            const std::string targetLower = lowerAscii(std::string(target));
            const std::string candidateLower = lowerAscii(std::string(candidate));
            return candidateLower == targetLower || candidateLower.find(targetLower) != std::string::npos;
        }

        bool npcMatches(std::string_view target, const ESM4::Npc& npc)
        {
            return targetMatches(target, npc.mEditorId) || targetMatches(target, npc.mFullName)
                || targetMatches(target, ESM::RefId(npc.mId).toDebugString());
        }

        bool creatureMatches(std::string_view target, const ESM4::Creature& creature)
        {
            return targetMatches(target, creature.mEditorId) || targetMatches(target, creature.mFullName)
                || targetMatches(target, ESM::RefId(creature.mId).toDebugString());
        }
    }

    AssetStudioWindow::AssetStudioWindow(osg::Group* parent, Resource::ResourceSystem* resourceSystem)
        : WindowBase("openmw_asset_studio_window.layout")
        , mParent(parent)
        , mResourceSystem(resourceSystem)
    {
        getWidget(mTitle, "TitleText");
        getWidget(mPreviewImage, "PreviewImage");
        getWidget(mAssetClassEdit, "AssetClassEdit");
        getWidget(mRecordEdit, "RecordEdit");
        getWidget(mSessionEdit, "SessionEdit");
        getWidget(mModelEdit, "ModelEdit");
        getWidget(mViewEdit, "ViewEdit");
        getWidget(mProfileEdit, "ProfileEdit");
        getWidget(mRotXEdit, "RotXEdit");
        getWidget(mRotYEdit, "RotYEdit");
        getWidget(mRotZEdit, "RotZEdit");
        getWidget(mScaleEdit, "ScaleEdit");
        getWidget(mZoomEdit, "ZoomEdit");
        getWidget(mApplyButton, "ApplyButton");
        getWidget(mStatus, "StatusText");
        getWidget(mCameraImages[0], "FrontPreviewImage");
        getWidget(mCameraImages[1], "LeftPreviewImage");
        getWidget(mCameraImages[2], "RightPreviewImage");

        mApplyButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onApply);
        mModelEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mAssetClassEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mRecordEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mSessionEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mViewEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mProfileEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mZoomEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);

        const std::string initialAssetClass = getenvTextOr("OPENMW_FNV_ASSET_STUDIO_ASSET_CLASS", "mesh");
        const std::string initialRecord = getenvTextOr("OPENMW_FNV_ASSET_STUDIO_RECORD", "direct-model");
        const std::string initialSession = getenvTextOr("OPENMW_FNV_ASSET_STUDIO_SESSION", "native-session");
        const std::string initialModel = getenvText("OPENMW_FNV_ASSET_STUDIO_MODEL");
        const std::string initialView = getenvTextOr("OPENMW_FNV_ASSET_STUDIO_VIEW", "front");
        std::string initialProfile = getenvText("OPENMW_FNV_ASSET_STUDIO_ACTOR_PROFILE");
        if (initialProfile.empty())
            initialProfile = getenvText("OPENMW_FNV_NEUTRAL_ACTOR_PREVIEW_PROFILE");

        mAssetClassEdit->setCaption(initialAssetClass);
        mRecordEdit->setCaption(initialRecord);
        mSessionEdit->setCaption(initialSession);
        mModelEdit->setCaption(initialModel);
        mViewEdit->setCaption(initialView);
        mProfileEdit->setCaption(initialProfile);
        mRotXEdit->setCaption(getenvTextOr("OPENMW_FNV_ASSET_STUDIO_ROT_X", "0"));
        mRotYEdit->setCaption(getenvTextOr("OPENMW_FNV_ASSET_STUDIO_ROT_Y", "0"));
        mRotZEdit->setCaption(getenvTextOr("OPENMW_FNV_ASSET_STUDIO_ROT_Z", "0"));
        mScaleEdit->setCaption(getenvTextOr("OPENMW_FNV_ASSET_STUDIO_SCALE", "1"));
        mZoomEdit->setCaption(getenvTextOr("OPENMW_FNV_ASSET_STUDIO_ZOOM", "1"));

        const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        setCoord(0, 0, viewSize.width, viewSize.height);
        mTitle->setCaption("FNV Asset Studio");
        setVisible(true);

        Log(Debug::Info) << "FNV/ESM4 asset studio native window opened model=\"" << initialModel
                         << "\" assetClass=\"" << initialAssetClass << "\" record=\"" << initialRecord
                         << "\" session=\"" << initialSession << "\" view=\"" << initialView
                         << "\" profile=\"" << initialProfile
                         << "\" gate=native-asset-studio-window cleanBackdrop=1";
        rebuildPreview();
    }

    AssetStudioWindow::~AssetStudioWindow() = default;

    void AssetStudioWindow::onResChange(int width, int height)
    {
        setCoord(0, 0, width, height);
        Log(Debug::Info) << "FNV/ESM4 asset studio resized width=" << width << " height=" << height
                         << " gate=native-asset-studio-clean-shell";
    }

    std::string AssetStudioWindow::editText(MyGUI::EditBox* edit) const
    {
        return edit != nullptr ? edit->getOnlyText().asUTF8() : std::string();
    }

    osg::Vec3f AssetStudioWindow::cameraDirectionForView(const std::string& view) const
    {
        if (view == "back")
            return osg::Vec3f(0.f, -1.f, 0.f);
        if (view == "left")
            return osg::Vec3f(1.f, 0.f, 0.f);
        if (view == "right")
            return osg::Vec3f(-1.f, 0.f, 0.f);
        if (view == "top")
            return osg::Vec3f(0.001f, 0.f, 1.f);
        if (view == "iso")
            return osg::Vec3f(0.45f, 0.75f, 0.45f);
        return osg::Vec3f(0.f, 1.f, 0.f);
    }

    MWRender::FalloutActorPreview::ViewMode AssetStudioWindow::actorViewModeForView(const std::string& view) const
    {
        const std::string lowered = lowerAscii(view);
        if (lowered == "left" || lowered == "front-left" || lowered == "face" || lowered == "face-hat")
            return MWRender::FalloutActorPreview::ViewMode::FrontLeft;
        if (lowered == "right" || lowered == "front-right" || lowered == "hands" || lowered == "weapon")
            return MWRender::FalloutActorPreview::ViewMode::FrontRight;
        return MWRender::FalloutActorPreview::ViewMode::Front;
    }

    void AssetStudioWindow::attachTexture(MyGUI::ImageBox* image, MyGUIPlatform::OSGTexture* texture) const
    {
        image->setRenderItemTexture(texture);
        image->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));
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

    bool AssetStudioWindow::isActorAssetClass(const std::string& assetClass) const
    {
        const std::string lowered = lowerAscii(assetClass);
        return lowered == "npc" || lowered == "person" || lowered == "actor" || lowered == "character"
            || lowered == "creature";
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
        const std::string assetClass = editText(mAssetClassEdit).empty() ? "mesh" : editText(mAssetClassEdit);
        const std::string record = editText(mRecordEdit).empty() ? "direct-model" : editText(mRecordEdit);
        const std::string session = editText(mSessionEdit).empty() ? "native-session" : editText(mSessionEdit);
        const std::string view = editText(mViewEdit).empty() ? "front" : editText(mViewEdit);
        const std::string profile = editText(mProfileEdit);
        const std::string model = editText(mModelEdit);
        const float zoom = editFloat(mZoomEdit, 1.f);

        Log(Debug::Info) << "FNV/ESM4 asset studio selector assetClass=\"" << assetClass << "\" record=\"" << record
                         << "\" session=\"" << session << "\" model=\"" << model << "\" view=\"" << view
                         << "\" profile=\"" << profile
                         << "\" zoom=" << zoom
                         << " gate=native-asset-studio-selector runtime=loaded-pending-runtime";

        if (isActorAssetClass(assetClass))
        {
            rebuildActorPreview(assetClass, record, session, view, profile, zoom);
            return;
        }

        rebuildModelPreview(assetClass, record, session, view, model, zoom);
    }

    bool AssetStudioWindow::rebuildActorPreview(const std::string& assetClass, const std::string& record,
        const std::string& session, const std::string& view, const std::string& profile, float zoom)
    {
        mPreview.reset();
        mPreviewTexture.reset();
        mPreviewImage->setRenderItemTexture(nullptr);
        for (std::size_t i = 0; i < mCameraPreviews.size(); ++i)
        {
            mCameraPreviews[i].reset();
            mCameraPreviewTextures[i].reset();
            mCameraImages[i]->setRenderItemTexture(nullptr);
        }

        try
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
                throw std::runtime_error("missing ESM store");

            const ESM4::Npc* npc = nullptr;
            const ESM4::Creature* creature = nullptr;
            int scannedNpcs = 0;
            int scannedCreatures = 0;
            if (lowerAscii(assetClass) != "creature")
            {
                for (const ESM4::Npc& candidate : store->get<ESM4::Npc>())
                {
                    ++scannedNpcs;
                    if (candidate.mIsFONV && npcMatches(record, candidate))
                    {
                        npc = &candidate;
                        break;
                    }
                }
            }
            if (npc == nullptr)
            {
                for (const ESM4::Creature& candidate : store->get<ESM4::Creature>())
                {
                    ++scannedCreatures;
                    if (creatureMatches(record, candidate))
                    {
                        creature = &candidate;
                        break;
                    }
                }
            }

            MWWorld::Ptr actor;
            std::string actorId;
            std::string fullName;
            std::string formId;
            std::string resolvedKind;
            if (npc != nullptr)
            {
                ESM::CellRef proxyRef;
                proxyRef.blank();
                proxyRef.mRefID = npc->mEditorId.empty() ? ESM::RefId(npc->mId) : ESM::RefId::stringRefId(npc->mEditorId);
                mActorNpcProxy = std::make_unique<MWWorld::LiveCellRef<ESM4::Npc>>(proxyRef, npc);
                mActorCreatureProxy.reset();
                actor = MWWorld::Ptr(mActorNpcProxy.get(), nullptr);
                actorId = npc->mEditorId;
                fullName = npc->mFullName;
                formId = ESM::RefId(npc->mId).toDebugString();
                resolvedKind = "npc";
            }
            else if (creature != nullptr)
            {
                ESM::CellRef proxyRef;
                proxyRef.blank();
                proxyRef.mRefID = creature->mEditorId.empty() ? ESM::RefId(creature->mId)
                                                              : ESM::RefId::stringRefId(creature->mEditorId);
                mActorCreatureProxy = std::make_unique<MWWorld::LiveCellRef<ESM4::Creature>>(proxyRef, creature);
                mActorNpcProxy.reset();
                actor = MWWorld::Ptr(mActorCreatureProxy.get(), nullptr);
                actorId = creature->mEditorId;
                fullName = creature->mFullName;
                formId = ESM::RefId(creature->mId).toDebugString();
                resolvedKind = "creature";
            }
            else
            {
                throw std::runtime_error("could not resolve actor record " + record);
            }

            const std::array<MWRender::FalloutActorPreview::ViewMode, 3> modes{
                MWRender::FalloutActorPreview::ViewMode::Front,
                MWRender::FalloutActorPreview::ViewMode::FrontLeft,
                MWRender::FalloutActorPreview::ViewMode::FrontRight,
            };

            mActorPreview = std::make_unique<MWRender::FalloutActorPreview>(
                mParent, mResourceSystem, actor, actorViewModeForView(view), zoom, profile);
            mActorPreview->rebuild();
            mActorPreview->redraw();
            mActorPreviewTexture = std::make_unique<MyGUIPlatform::OSGTexture>(
                mActorPreview->getTexture().get(), mActorPreview->getTextureStateSet());
            attachTexture(mPreviewImage, mActorPreviewTexture.get());

            for (std::size_t i = 0; i < modes.size(); ++i)
            {
                mActorCameraPreviews[i]
                    = std::make_unique<MWRender::FalloutActorPreview>(mParent, mResourceSystem, actor, modes[i], zoom, profile);
                mActorCameraPreviews[i]->rebuild();
                mActorCameraPreviews[i]->redraw();
                mActorCameraPreviewTextures[i] = std::make_unique<MyGUIPlatform::OSGTexture>(
                    mActorCameraPreviews[i]->getTexture().get(), mActorCameraPreviews[i]->getTextureStateSet());
                attachTexture(mCameraImages[i], mActorCameraPreviewTextures[i].get());
            }

            setStatus("actor loaded " + actorId);
            Log(Debug::Info) << "FNV/ESM4 asset studio actor loaded assetClass=\"" << assetClass << "\" record=\""
                             << record << "\" resolvedKind=\"" << resolvedKind << "\" actor=\"" << actorId
                             << "\" full=\"" << fullName << "\" form=" << formId << " session=\"" << session
                             << "\" view=\"" << view << "\" profile=\"" << profile << "\" zoom=" << zoom
                             << " panes=4 threeCamera=1"
                             << " scannedNpcBases=" << scannedNpcs << " scannedCreatureBases=" << scannedCreatures
                             << " gate=native-asset-studio-actor-preview runtime=runtime-supported";
            Log(Debug::Info) << "FNV/ESM4 asset studio three camera actor preview assetClass=\"" << assetClass
                             << "\" record=\"" << record << "\" actor=\"" << actorId
                             << "\" views=\"front,front-left,front-right\" profile=\"" << profile << "\" zoom=" << zoom
                             << " gate=native-asset-studio-three-camera-actor-preview runtime=runtime-supported";
            return true;
        }
        catch (const std::exception& e)
        {
            mActorPreview.reset();
            mActorPreviewTexture.reset();
            mActorNpcProxy.reset();
            mActorCreatureProxy.reset();
            mPreviewImage->setRenderItemTexture(nullptr);
            for (std::size_t i = 0; i < mActorCameraPreviews.size(); ++i)
            {
                mActorCameraPreviews[i].reset();
                mActorCameraPreviewTextures[i].reset();
                mCameraImages[i]->setRenderItemTexture(nullptr);
            }
            setStatus(std::string("actor failed: ") + e.what());
            Log(Debug::Warning) << "FNV/ESM4 asset studio actor failed assetClass=\"" << assetClass
                                << "\" record=\"" << record << "\" session=\"" << session << "\" view=\"" << view
                                << "\" profile=\"" << profile << "\" zoom=" << zoom << " error=\"" << e.what()
                                << "\" gate=native-asset-studio-actor-preview runtime=known-blocked";
            return false;
        }
    }

    bool AssetStudioWindow::rebuildModelPreview(const std::string& assetClass, const std::string& record,
        const std::string& session, const std::string& view, const std::string& model, float zoom)
    {
        mActorPreview.reset();
        mActorPreviewTexture.reset();
        mActorNpcProxy.reset();
        mActorCreatureProxy.reset();
        for (std::size_t i = 0; i < mActorCameraPreviews.size(); ++i)
        {
            mActorCameraPreviews[i].reset();
            mActorCameraPreviewTextures[i].reset();
        }

        if (model.empty())
        {
            mPreview.reset();
            mPreviewTexture.reset();
            mPreviewImage->setRenderItemTexture(nullptr);
            for (std::size_t i = 0; i < mCameraPreviews.size(); ++i)
            {
                mCameraPreviews[i].reset();
                mCameraPreviewTextures[i].reset();
                mCameraImages[i]->setRenderItemTexture(nullptr);
            }
            setStatus("Enter a model path, then Apply.");
            Log(Debug::Info) << "FNV/ESM4 asset studio idle reason=no-model assetClass=\"" << assetClass
                             << "\" record=\"" << record
                             << "\" gate=native-asset-studio-model-preview runtime=loaded-pending-runtime";
            return false;
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
            settings.mCameraDirection = cameraDirectionForView(view);
            settings.mCameraDistanceMultiplier = zoom;

            mPreview = std::make_unique<MWRender::StandaloneModelPreview>(mParent, mResourceSystem, settings);
            mPreviewTexture = std::make_unique<MyGUIPlatform::OSGTexture>(mPreview->getTexture());
            attachTexture(mPreviewImage, mPreviewTexture.get());

            const std::array<std::string, 3> auditViews{ "front", "left", "right" };
            for (std::size_t i = 0; i < auditViews.size(); ++i)
            {
                MWRender::StandaloneModelPreviewSettings cameraSettings = settings;
                cameraSettings.mWidth = 512;
                cameraSettings.mHeight = 384;
                cameraSettings.mCameraDirection = cameraDirectionForView(auditViews[i]);
                mCameraPreviews[i]
                    = std::make_unique<MWRender::StandaloneModelPreview>(mParent, mResourceSystem, cameraSettings);
                mCameraPreviewTextures[i] = std::make_unique<MyGUIPlatform::OSGTexture>(mCameraPreviews[i]->getTexture());
                attachTexture(mCameraImages[i], mCameraPreviewTextures[i].get());
            }

            const MWRender::StandaloneModelPreviewState& state = mPreview->getState();
            setStatus(std::string("loaded bounds=") + (state.mBoundsValid ? "valid" : "invalid"));
            Log(Debug::Info) << "FNV/ESM4 asset studio model loaded assetClass=\"" << assetClass << "\" record=\""
                             << record << "\" session=\"" << session << "\" view=\"" << view << "\" model=\"" << model
                             << "\" correctedModel=\"" << state.mCorrectedModel << "\" boundsValid="
                             << state.mBoundsValid << " boundsSize=(" << state.mBoundsSize.x() << ","
                             << state.mBoundsSize.y() << "," << state.mBoundsSize.z()
                             << ") camera=(" << state.mCameraPosition.x() << "," << state.mCameraPosition.y()
                             << "," << state.mCameraPosition.z()
                             << ") zoom=" << zoom << " threeCamera=1"
                             << " gate=native-asset-studio-model-preview runtime=runtime-supported";
            Log(Debug::Info) << "FNV/ESM4 asset studio three camera preview assetClass=\"" << assetClass
                             << "\" record=\"" << record << "\" session=\"" << session
                             << "\" views=\"front,left,right\" zoom=" << zoom
                             << " gate=native-asset-studio-three-camera-preview runtime=runtime-supported";
            return true;
        }
        catch (const std::exception& e)
        {
            mPreview.reset();
            mPreviewTexture.reset();
            mPreviewImage->setRenderItemTexture(nullptr);
            for (std::size_t i = 0; i < mCameraPreviews.size(); ++i)
            {
                mCameraPreviews[i].reset();
                mCameraPreviewTextures[i].reset();
                mCameraImages[i]->setRenderItemTexture(nullptr);
            }
            setStatus(std::string("failed: ") + e.what());
            Log(Debug::Warning) << "FNV/ESM4 asset studio model failed assetClass=\"" << assetClass
                                << "\" record=\"" << record << "\" session=\"" << session << "\" view=\"" << view
                                << "\" model=\"" << model << "\" zoom=" << zoom << " error=\"" << e.what()
                                << "\" gate=native-asset-studio-model-preview runtime=known-blocked";
            return false;
        }
    }
}
