#include "assetstudiowindow.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_Widget.h>

#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadflor.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/loadweap.hpp>
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

        std::string falloutFormToken(std::string_view value)
        {
            while (!value.empty()
                && (std::isspace(static_cast<unsigned char>(value.front())) || value.front() == '"'
                    || value.front() == '\''))
                value.remove_prefix(1);
            while (!value.empty()
                && (std::isspace(static_cast<unsigned char>(value.back())) || value.back() == '"'
                    || value.back() == '\''))
                value.remove_suffix(1);

            std::string text = lowerAscii(std::string(value));
            constexpr std::string_view prefix = "formid:";
            if (text.rfind(prefix, 0) == 0)
                text.erase(0, prefix.size());
            if (text.rfind("0x", 0) == 0)
                text.erase(0, 2);
            if (text.empty())
                return {};
            if (!std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
                return {};
            if (text.size() > 6)
                text.erase(0, text.size() - 6);
            while (text.size() < 6)
                text.insert(text.begin(), '0');
            return text;
        }

        bool falloutFormTargetMatches(std::string_view target, std::string_view candidate)
        {
            const std::string targetToken = falloutFormToken(target);
            return !targetToken.empty() && targetToken == falloutFormToken(candidate);
        }

        bool targetMatches(std::string_view target, std::string_view candidate)
        {
            if (target.empty() || candidate.empty())
                return false;

            const std::string targetLower = lowerAscii(std::string(target));
            const std::string candidateLower = lowerAscii(std::string(candidate));
            return candidateLower == targetLower || falloutFormTargetMatches(target, candidate)
                || candidateLower.find(targetLower) != std::string::npos
                || targetLower.find(candidateLower) != std::string::npos;
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

        bool classMatches(const std::string& lowered, std::initializer_list<std::string_view> aliases)
        {
            for (std::string_view alias : aliases)
            {
                if (lowered == alias)
                    return true;
            }
            return false;
        }

        bool isRecordModelAssetClass(std::string_view assetClass)
        {
            const std::string lowered = lowerAscii(std::string(assetClass));
            return classMatches(lowered,
                { "acti", "activator", "activators", "alch", "potion", "potions", "ammo", "ammunition", "armo",
                    "armor", "armour", "book", "books", "cont", "container", "containers", "door", "doors",
                    "flor", "flora", "furn", "furniture", "ingr", "ingredient", "ingredients", "keym", "key",
                    "keys", "ligh", "light", "lights", "misc", "misc-item", "miscitem", "misc-items", "item",
                    "items", "object", "objects", "prop", "props", "record", "model-record", "stat", "static",
                    "statics", "weap", "weapon", "weapons" });
        }

        std::string firstNonEmpty(std::initializer_list<std::string_view> values)
        {
            for (std::string_view value : values)
            {
                if (!value.empty())
                    return std::string(value);
            }
            return {};
        }

        template <class T>
        bool recordMatches(std::string_view target, const T& record)
        {
            return targetMatches(target, record.mEditorId) || targetMatches(target, record.mFullName)
                || targetMatches(target, ESM::RefId(record.mId).toDebugString());
        }

        struct RecordModelMatch
        {
            bool mFound = false;
            int mScanned = 0;
            std::string mKind;
            std::string mEditorId;
            std::string mFullName;
            std::string mFormId;
            std::string mModel;
        };

        template <class T, class ModelFn>
        RecordModelMatch findRecordModel(const MWWorld::ESMStore& store, std::string_view target, std::string kind,
            ModelFn&& modelFn)
        {
            RecordModelMatch result;
            result.mKind = std::move(kind);
            for (const T& candidate : store.get<T>())
            {
                ++result.mScanned;
                if (!recordMatches(target, candidate))
                    continue;

                result.mFound = true;
                result.mEditorId = candidate.mEditorId;
                result.mFullName = candidate.mFullName;
                result.mFormId = ESM::RefId(candidate.mId).toDebugString();
                result.mModel = std::string(modelFn(candidate));
                return result;
            }
            return result;
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
        if (lowered == "left")
            return MWRender::FalloutActorPreview::ViewMode::Left;
        if (lowered == "right")
            return MWRender::FalloutActorPreview::ViewMode::Right;
        if (lowered == "top")
            return MWRender::FalloutActorPreview::ViewMode::Top;
        if (lowered == "front-left" || lowered == "face" || lowered == "face-hat")
            return MWRender::FalloutActorPreview::ViewMode::FrontLeft;
        if (lowered == "front-right" || lowered == "hands" || lowered == "weapon")
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
        std::string model = editText(mModelEdit);
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

        if (model.empty())
        {
            model = resolveRecordModel(assetClass, record);
            if (!model.empty())
                mModelEdit->setCaption(model);
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

    std::string AssetStudioWindow::resolveRecordModel(const std::string& assetClass, const std::string& record) const
    {
        if (!isRecordModelAssetClass(assetClass))
            return {};

        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr)
        {
            Log(Debug::Warning) << "FNV/ESM4 asset studio record model failed assetClass=\"" << assetClass
                                << "\" record=\"" << record << "\" error=\"missing ESM store\""
                                << " gate=native-asset-studio-record-model runtime=known-blocked";
            return {};
        }

        const std::string lowered = lowerAscii(assetClass);
        RecordModelMatch match;

        auto scanActivator = [&] {
            return findRecordModel<ESM4::Activator>(
                *store, record, "activator", [](const ESM4::Activator& value) { return value.mModel; });
        };
        auto scanPotion = [&] {
            return findRecordModel<ESM4::Potion>(
                *store, record, "potion", [](const ESM4::Potion& value) { return value.mModel; });
        };
        auto scanAmmo = [&] {
            return findRecordModel<ESM4::Ammunition>(
                *store, record, "ammo", [](const ESM4::Ammunition& value) { return value.mModel; });
        };
        auto scanArmor = [&] {
            return findRecordModel<ESM4::Armor>(*store, record, "armor", [](const ESM4::Armor& value) {
                return firstNonEmpty(
                    { value.mModelMaleWorld, value.mModelFemaleWorld, value.mModelMale, value.mModelFemale, value.mModel });
            });
        };
        auto scanBook = [&] {
            return findRecordModel<ESM4::Book>(
                *store, record, "book", [](const ESM4::Book& value) { return value.mModel; });
        };
        auto scanContainer = [&] {
            return findRecordModel<ESM4::Container>(
                *store, record, "container", [](const ESM4::Container& value) { return value.mModel; });
        };
        auto scanDoor = [&] {
            return findRecordModel<ESM4::Door>(
                *store, record, "door", [](const ESM4::Door& value) { return value.mModel; });
        };
        auto scanFlora = [&] {
            return findRecordModel<ESM4::Flora>(
                *store, record, "flora", [](const ESM4::Flora& value) { return value.mModel; });
        };
        auto scanFurniture = [&] {
            return findRecordModel<ESM4::Furniture>(
                *store, record, "furniture", [](const ESM4::Furniture& value) { return value.mModel; });
        };
        auto scanIngredient = [&] {
            return findRecordModel<ESM4::Ingredient>(
                *store, record, "ingredient", [](const ESM4::Ingredient& value) { return value.mModel; });
        };
        auto scanLight = [&] {
            return findRecordModel<ESM4::Light>(
                *store, record, "light", [](const ESM4::Light& value) { return value.mModel; });
        };
        auto scanMisc = [&] {
            return findRecordModel<ESM4::MiscItem>(
                *store, record, "misc-item", [](const ESM4::MiscItem& value) { return value.mModel; });
        };
        auto scanStatic = [&] {
            return findRecordModel<ESM4::Static>(
                *store, record, "static", [](const ESM4::Static& value) { return value.mModel; });
        };
        auto scanWeapon = [&] {
            return findRecordModel<ESM4::Weapon>(
                *store, record, "weapon", [](const ESM4::Weapon& value) { return value.mModel; });
        };

        if (classMatches(lowered, { "acti", "activator", "activators" }))
            match = scanActivator();
        else if (classMatches(lowered, { "alch", "potion", "potions" }))
            match = scanPotion();
        else if (classMatches(lowered, { "ammo", "ammunition" }))
            match = scanAmmo();
        else if (classMatches(lowered, { "armo", "armor", "armour" }))
            match = scanArmor();
        else if (classMatches(lowered, { "book", "books" }))
            match = scanBook();
        else if (classMatches(lowered, { "cont", "container", "containers" }))
            match = scanContainer();
        else if (classMatches(lowered, { "door", "doors" }))
            match = scanDoor();
        else if (classMatches(lowered, { "flor", "flora" }))
            match = scanFlora();
        else if (classMatches(lowered, { "furn", "furniture" }))
            match = scanFurniture();
        else if (classMatches(lowered, { "ingr", "ingredient", "ingredients" }))
            match = scanIngredient();
        else if (classMatches(lowered, { "keym", "key", "keys" }))
        {
            Log(Debug::Warning) << "FNV/ESM4 asset studio record model failed assetClass=\"" << assetClass
                                << "\" record=\"" << record << "\" resolvedKind=\"key\" scanned=0 foundRecord=0"
                                << " model=\"\" error=\"ESM4::Key is not registered in ESMStore\""
                                << " gate=native-asset-studio-record-model runtime=known-blocked";
            return {};
        }
        else if (classMatches(lowered, { "ligh", "light", "lights" }))
            match = scanLight();
        else if (classMatches(lowered, { "misc", "misc-item", "miscitem", "misc-items" }))
            match = scanMisc();
        else if (classMatches(lowered, { "stat", "static", "statics" }))
            match = scanStatic();
        else if (classMatches(lowered, { "weap", "weapon", "weapons" }))
            match = scanWeapon();
        else
        {
            const std::array<RecordModelMatch, 14> scans{ scanActivator(), scanPotion(), scanAmmo(), scanArmor(),
                scanBook(), scanContainer(), scanDoor(), scanFlora(), scanFurniture(), scanIngredient(), scanLight(),
                scanMisc(), scanStatic(), scanWeapon() };
            for (const RecordModelMatch& candidate : scans)
            {
                if (candidate.mFound)
                {
                    match = candidate;
                    break;
                }
                match.mScanned += candidate.mScanned;
            }
        }

        if (match.mFound && !match.mModel.empty())
        {
            Log(Debug::Info) << "FNV/ESM4 asset studio record model resolved assetClass=\"" << assetClass
                             << "\" record=\"" << record << "\" resolvedKind=\"" << match.mKind << "\" editorId=\""
                             << match.mEditorId << "\" full=\"" << match.mFullName << "\" form=" << match.mFormId
                             << " model=\"" << match.mModel << "\" scanned=" << match.mScanned
                             << " gate=native-asset-studio-record-model runtime=loaded-pending-runtime";
            return match.mModel;
        }

        Log(Debug::Warning) << "FNV/ESM4 asset studio record model failed assetClass=\"" << assetClass
                            << "\" record=\"" << record << "\" resolvedKind=\"" << match.mKind << "\" scanned="
                            << match.mScanned << " foundRecord=" << match.mFound << " model=\"" << match.mModel
                            << "\" gate=native-asset-studio-record-model runtime=known-blocked";
        return {};
    }
}
