#include "assetstudiowindow.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_ComboBox.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_ListBox.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollBar.h>
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

        bool getenvBool(const char* name)
        {
            return std::getenv(name) != nullptr;
        }

        void setProcessEnv(const char* name, bool enabled)
        {
#ifdef _WIN32
            _putenv_s(name, enabled ? "1" : "");
#else
            if (enabled)
                setenv(name, "1", 1);
            else
                unsetenv(name);
#endif
        }

        void setProcessEnvText(const char* name, const std::string& value)
        {
#ifdef _WIN32
            _putenv_s(name, value.c_str());
#else
            if (value.empty())
                unsetenv(name);
            else
                setenv(name, value.c_str(), 1);
#endif
        }

        std::string jsonEscape(std::string_view value)
        {
            std::string result;
            result.reserve(value.size() + 8);
            for (char ch : value)
            {
                switch (ch)
                {
                    case '\\':
                        result += "\\\\";
                        break;
                    case '"':
                        result += "\\\"";
                        break;
                    case '\n':
                        result += "\\n";
                        break;
                    case '\r':
                        result += "\\r";
                        break;
                    case '\t':
                        result += "\\t";
                        break;
                    default:
                        result += ch;
                        break;
                }
            }
            return result;
        }

        std::string jsonUnescape(std::string_view value)
        {
            std::string result;
            result.reserve(value.size());
            bool escaped = false;
            for (char ch : value)
            {
                if (escaped)
                {
                    switch (ch)
                    {
                        case 'n':
                            result += '\n';
                            break;
                        case 'r':
                            result += '\r';
                            break;
                        case 't':
                            result += '\t';
                            break;
                        default:
                            result += ch;
                            break;
                    }
                    escaped = false;
                }
                else if (ch == '\\')
                    escaped = true;
                else
                    result += ch;
            }
            if (escaped)
                result += '\\';
            return result;
        }

        bool readJsonStringField(std::string_view content, std::string_view key, std::string& value)
        {
            const std::string needle = "\"" + std::string(key) + "\"";
            std::size_t pos = content.find(needle);
            if (pos == std::string_view::npos)
                return false;
            pos = content.find(':', pos + needle.size());
            if (pos == std::string_view::npos)
                return false;
            pos = content.find('"', pos + 1);
            if (pos == std::string_view::npos)
                return false;

            std::string raw;
            bool escaped = false;
            for (++pos; pos < content.size(); ++pos)
            {
                const char ch = content[pos];
                if (escaped)
                {
                    raw += '\\';
                    raw += ch;
                    escaped = false;
                }
                else if (ch == '\\')
                    escaped = true;
                else if (ch == '"')
                {
                    value = jsonUnescape(raw);
                    return true;
                }
                else
                    raw += ch;
            }
            return false;
        }

        bool readWholeFile(const char* path, std::string& content)
        {
            if (path == nullptr || path[0] == '\0')
                return false;
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            std::ostringstream stream;
            stream << file.rdbuf();
            content = stream.str();
            return true;
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

        bool isLiveTransformField(std::string_view field)
        {
            const std::string lowered = lowerAscii(std::string(field));
            return lowered == "zoom" || lowered == "panx" || lowered == "pan-x" || lowered == "panz"
                || lowered == "pan-z" || lowered == "tilt" || lowered == "tiltdeg" || lowered == "tilt-deg"
                || lowered == "rotx" || lowered == "rot-x" || lowered == "roty" || lowered == "rot-y"
                || lowered == "rotz" || lowered == "rot-z" || lowered == "scale";
        }

        float clampFinite(float value, float fallback, float minValue, float maxValue)
        {
            if (!std::isfinite(value))
                value = fallback;
            return std::clamp(value, minValue, maxValue);
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

        std::string catalogText(std::string_view assetClass, std::string_view fullName, std::string_view editorId,
            std::string_view formId, std::string_view model)
        {
            const std::string name = firstNonEmpty({ fullName, editorId, formId, model });
            std::string result = std::string(assetClass) + "  " + name;
            if (!editorId.empty() && editorId != name)
                result += "  [" + std::string(editorId) + "]";
            if (!model.empty())
                result += "  -> " + std::string(model);
            return result;
        }

        std::string catalogData(std::string_view assetClass, std::string_view record, std::string_view model)
        {
            return std::string(assetClass) + "\x1f" + std::string(record) + "\x1f" + std::string(model);
        }

        bool splitCatalogData(std::string_view data, std::string& assetClass, std::string& record, std::string& model)
        {
            const std::size_t first = data.find('\x1f');
            const std::size_t second = first == std::string_view::npos ? std::string_view::npos : data.find('\x1f', first + 1);
            if (first == std::string_view::npos || second == std::string_view::npos)
                return false;
            assetClass = std::string(data.substr(0, first));
            record = std::string(data.substr(first + 1, second - first - 1));
            model = std::string(data.substr(second + 1));
            return true;
        }

        bool catalogFilterMatches(const std::string& loweredFilter, std::initializer_list<std::string_view> values)
        {
            if (loweredFilter.empty())
                return true;
            for (std::string_view value : values)
            {
                if (lowerAscii(std::string(value)).find(loweredFilter) != std::string::npos)
                    return true;
            }
            return false;
        }

        constexpr std::array<std::string_view, 8> sCameraTileViews{
            "front", "back", "left", "right", "top", "bottom", "iso-nw", "iso-sw"
        };

        constexpr std::array<std::string_view, 8> sCameraImageNames{
            "FrontPreviewImage", "BackPreviewImage", "LeftPreviewImage", "RightPreviewImage",
            "TopPreviewImage", "BottomPreviewImage", "IsoNWPreviewImage", "IsoSWPreviewImage"
        };

        constexpr std::array<std::string_view, 8> sCameraHotspotNames{
            "FrontTileHotspot", "BackTileHotspot", "LeftTileHotspot", "RightTileHotspot",
            "TopTileHotspot", "BottomTileHotspot", "IsoNWTileHotspot", "IsoSWTileHotspot"
        };

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

        struct CatalogItem
        {
            std::string mAssetClass;
            std::string mRecord;
            std::string mLabel;
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

        template <class T, class ModelFn>
        void appendRecordCatalog(std::vector<CatalogItem>& items, const MWWorld::ESMStore& store,
            std::string_view assetClass, const std::string& loweredFilter, int& scanned, ModelFn&& modelFn)
        {
            for (const T& candidate : store.get<T>())
            {
                ++scanned;
                const std::string formId = ESM::RefId(candidate.mId).toDebugString();
                const std::string model = std::string(modelFn(candidate));
                if (!catalogFilterMatches(loweredFilter, { assetClass, candidate.mFullName, candidate.mEditorId, formId, model }))
                    continue;

                const std::string record = firstNonEmpty({ candidate.mEditorId, candidate.mFullName, formId });
                items.push_back(CatalogItem{ std::string(assetClass), record,
                    catalogText(assetClass, candidate.mFullName, candidate.mEditorId, formId, model), model });
            }
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
        getWidget(mAssetCategoryCombo, "AssetCategoryCombo");
        getWidget(mAssetCategoryList, "AssetCategoryList");
        getWidget(mAssetSearchEdit, "AssetSearchEdit");
        getWidget(mAssetResultList, "AssetResultList");
        getWidget(mAssetCountText, "AssetCountText");
        getWidget(mAssetLoadButton, "AssetLoadButton");
        getWidget(mScreenshotButton, "ScreenshotButton");
        getWidget(mViewEdit, "ViewEdit");
        getWidget(mProfileEdit, "ProfileEdit");
        getWidget(mRotXEdit, "RotXEdit");
        getWidget(mRotYEdit, "RotYEdit");
        getWidget(mRotZEdit, "RotZEdit");
        getWidget(mScaleEdit, "ScaleEdit");
        getWidget(mZoomEdit, "ZoomEdit");
        getWidget(mPanXEdit, "PanXEdit");
        getWidget(mPanZEdit, "PanZEdit");
        getWidget(mTiltEdit, "TiltEdit");
        getWidget(mRotXSlider, "RotXSlider");
        getWidget(mRotYSlider, "RotYSlider");
        getWidget(mRotZSlider, "RotZSlider");
        getWidget(mScaleSlider, "ScaleSlider");
        getWidget(mZoomSlider, "ZoomSlider");
        getWidget(mApplyButton, "ApplyButton");
        getWidget(mSaveButton, "SaveButton");
        getWidget(mSnapXNegButton, "SnapXNegButton");
        getWidget(mSnapXZeroButton, "SnapXZeroButton");
        getWidget(mSnapXPosButton, "SnapXPosButton");
        getWidget(mSnapYNegButton, "SnapYNegButton");
        getWidget(mSnapYPosButton, "SnapYPosButton");
        getWidget(mSnapZPosButton, "SnapZPosButton");
        getWidget(mViewFrontButton, "ViewFrontButton");
        getWidget(mViewLeftButton, "ViewLeftButton");
        getWidget(mViewTopButton, "ViewTopButton");
        getWidget(mProfileFaceButton, "ProfileFaceButton");
        getWidget(mProfileBodyButton, "ProfileBodyButton");
        getWidget(mProfileHandsButton, "ProfileHandsButton");
        getWidget(mProfileWeaponButton, "ProfileWeaponButton");
        getWidget(mAxesButton, "AxesButton");
        getWidget(mIkButton, "IkButton");
        getWidget(mWireframeButton, "WireframeButton");
        getWidget(mWeightsButton, "WeightsButton");
        getWidget(mBloodButton, "BloodButton");
        getWidget(mInventoryButton, "InventoryButton");
        getWidget(mEquipmentButton, "EquipmentButton");
        getWidget(mStatus, "StatusText");
        for (std::size_t i = 0; i < mCameraImages.size(); ++i)
        {
            getWidget(mCameraImages[i], std::string(sCameraImageNames[i]));
            getWidget(mCameraHotspots[i], std::string(sCameraHotspotNames[i]));
        }

        mApplyButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onApply);
        mSaveButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onSave);
        mSnapXNegButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onSnap);
        mSnapXZeroButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onSnap);
        mSnapXPosButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onSnap);
        mSnapYNegButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onSnap);
        mSnapYPosButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onSnap);
        mSnapZPosButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onSnap);
        mViewFrontButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onViewButton);
        mViewLeftButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onViewButton);
        mViewTopButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onViewButton);
        for (std::size_t i = 0; i < mCameraImages.size(); ++i)
        {
            mCameraImages[i]->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onViewButton);
            mCameraHotspots[i]->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onViewButton);
        }
        mProfileFaceButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onProfileButton);
        mProfileBodyButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onProfileButton);
        mProfileHandsButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onProfileButton);
        mProfileWeaponButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onProfileButton);
        mAxesButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onDebugButton);
        mIkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onDebugButton);
        mWireframeButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onDebugButton);
        mWeightsButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onDebugButton);
        mBloodButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onDebugButton);
        mInventoryButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onDebugButton);
        mEquipmentButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onDebugButton);
        for (MyGUI::Widget* widget : { static_cast<MyGUI::Widget*>(mAssetCategoryCombo),
                 static_cast<MyGUI::Widget*>(mAssetCategoryList), static_cast<MyGUI::Widget*>(mAssetSearchEdit), static_cast<MyGUI::Widget*>(mAssetResultList),
                 static_cast<MyGUI::Widget*>(mAssetLoadButton), static_cast<MyGUI::Widget*>(mScreenshotButton) })
        {
            if (widget != nullptr)
                widget->setNeedMouseFocus(true);
        }
        mAssetCategoryCombo->eventComboChangePosition
            += MyGUI::newDelegate(this, &AssetStudioWindow::onAssetCategoryChanged);
        mAssetCategoryList->eventListChangePosition
            += MyGUI::newDelegate(this, &AssetStudioWindow::onAssetCategoryListChanged);
        mAssetSearchEdit->eventEditTextChange += MyGUI::newDelegate(this, &AssetStudioWindow::onAssetSearchChanged);
        mAssetResultList->eventListChangePosition += MyGUI::newDelegate(this, &AssetStudioWindow::onAssetSelected);
        mAssetResultList->eventListSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAssetAccepted);
        mAssetLoadButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onAssetLoad);
        mScreenshotButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AssetStudioWindow::onScreenshot);
        mModelEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mAssetClassEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mRecordEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mSessionEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mViewEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mProfileEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mZoomEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mPanXEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mPanZEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mTiltEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mRotXEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mRotYEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mRotZEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mScaleEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AssetStudioWindow::onAccept);
        mRotXSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &AssetStudioWindow::onSliderMoved);
        mRotYSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &AssetStudioWindow::onSliderMoved);
        mRotZSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &AssetStudioWindow::onSliderMoved);
        mScaleSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &AssetStudioWindow::onSliderMoved);
        mZoomSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &AssetStudioWindow::onSliderMoved);

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
        mPanXEdit->setCaption(getenvTextOr("OPENMW_FNV_ASSET_STUDIO_CAMERA_PAN_X", "0"));
        mPanZEdit->setCaption(getenvTextOr("OPENMW_FNV_ASSET_STUDIO_CAMERA_PAN_Z", "0"));
        mTiltEdit->setCaption(getenvTextOr("OPENMW_FNV_ASSET_STUDIO_CAMERA_TILT_DEG", "0"));
        populateAssetCategories();
        updateAssetResults();
        setProcessEnv("OPENMW_FNV_DISABLE_WEAPON_IK", true);
        setProcessEnv("OPENMW_FNV_BONE_IK_DEBUG", false);
        setProcessEnv("OPENMW_FNV_SHOW_IK_BONES", false);
        setProcessEnv("OPENMW_FNV_DRAW_PART_AXES", false);
        setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME", false);
        setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS", false);
        setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS", false);
        setProcessEnvText("OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE", "");
        setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_HIDE_BLOOD", true);
        mAxesEnabled = false;
        mIkEnabled = false;
        mWireframeEnabled = false;
        mWeightsEnabled = false;
        mBloodVisible = false;
        configureSliders();
        syncSlidersFromEdits();
        updateToggleCaptions();

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

    void AssetStudioWindow::onFrame(float duration)
    {
        pollCommandFile(duration);
        if (mRetiredPreviewSeconds > 0.f)
        {
            mRetiredPreviewSeconds -= duration;
            if (mRetiredPreviewSeconds <= 0.f)
                clearRetiredPreviews();
        }
        if (mPendingPreviewSeconds > 0.f || mPendingPreviewFrames > 0)
        {
            if (mPendingPreviewSeconds > 0.f)
                mPendingPreviewSeconds -= duration;
            if (mPendingPreviewFrames > 0)
                --mPendingPreviewFrames;
            if (mPendingPreviewSeconds <= 0.f && mPendingPreviewFrames <= 0)
                promotePendingPreviews();
        }
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
        if (view == "bottom")
            return osg::Vec3f(0.001f, 0.f, -1.f);
        if (view == "iso" || view == "iso-nw")
            return osg::Vec3f(0.45f, 0.75f, 0.45f);
        if (view == "iso-sw")
            return osg::Vec3f(0.45f, -0.75f, 0.45f);
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
        if (lowered == "bottom")
            return MWRender::FalloutActorPreview::ViewMode::Bottom;
        if (lowered == "back")
            return MWRender::FalloutActorPreview::ViewMode::Back;
        if (lowered == "iso" || lowered == "iso-nw")
            return MWRender::FalloutActorPreview::ViewMode::IsoNW;
        if (lowered == "iso-sw")
            return MWRender::FalloutActorPreview::ViewMode::IsoSW;
        if (lowered == "front-left" || lowered == "face" || lowered == "face-hat")
            return MWRender::FalloutActorPreview::ViewMode::FrontLeft;
        if (lowered == "front-right" || lowered == "hands" || lowered == "weapon")
            return MWRender::FalloutActorPreview::ViewMode::FrontRight;
        return MWRender::FalloutActorPreview::ViewMode::Front;
    }

    std::string AssetStudioWindow::cameraTileView(std::size_t index) const
    {
        return index < sCameraTileViews.size() ? std::string(sCameraTileViews[index]) : std::string("front");
    }

    bool AssetStudioWindow::promoteCameraTile(const std::string& view)
    {
        const std::string lowered = lowerAscii(view);
        for (std::size_t i = 0; i < sCameraTileViews.size(); ++i)
        {
            if (lowerAscii(std::string(sCameraTileViews[i])) != lowered)
                continue;

            if (mActorCameraPreviewTextures[i] != nullptr)
            {
                attachTexture(mPreviewImage, mActorCameraPreviewTextures[i].get());
                return true;
            }
            if (mCameraPreviewTextures[i] != nullptr)
            {
                attachTexture(mPreviewImage, mCameraPreviewTextures[i].get());
                return true;
            }
            return false;
        }
        return false;
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
        if (!std::isfinite(parsed))
            return fallback;
        return parsed;
    }

    void AssetStudioWindow::setEditFloat(MyGUI::EditBox* edit, float value)
    {
        if (edit == nullptr)
            return;

        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3) << value;
        std::string text = stream.str();
        while (text.size() > 1 && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
        edit->setCaption(text);
    }

    void AssetStudioWindow::configureSliders()
    {
        mSyncingSliders = true;
        for (MyGUI::ScrollBar* slider : { mRotXSlider, mRotYSlider, mRotZSlider })
        {
            slider->setScrollRange(361);
            slider->setScrollPage(15);
        }
        mScaleSlider->setScrollRange(291);
        mScaleSlider->setScrollPage(10);
        mZoomSlider->setScrollRange(1191);
        mZoomSlider->setScrollPage(25);
        mSyncingSliders = false;
    }

    void AssetStudioWindow::setSliderFromFloat(
        MyGUI::ScrollBar* slider, float value, float minValue, float maxValue, float step)
    {
        if (slider == nullptr)
            return;
        const float clamped = std::clamp(value, minValue, maxValue);
        const int position = static_cast<int>(std::lround((clamped - minValue) / step));
        const int maxPosition = static_cast<int>(slider->getScrollRange()) - 1;
        slider->setScrollPosition(static_cast<size_t>(std::clamp(position, 0, maxPosition)));
    }

    float AssetStudioWindow::sliderFloat(MyGUI::ScrollBar* slider, float minValue, float step) const
    {
        if (slider == nullptr)
            return minValue;
        return minValue + static_cast<float>(slider->getScrollPosition()) * step;
    }

    void AssetStudioWindow::syncSlidersFromEdits()
    {
        mSyncingSliders = true;
        setSliderFromFloat(mRotXSlider, editFloat(mRotXEdit, 0.f), -180.f, 180.f, 1.f);
        setSliderFromFloat(mRotYSlider, editFloat(mRotYEdit, 0.f), -180.f, 180.f, 1.f);
        setSliderFromFloat(mRotZSlider, editFloat(mRotZEdit, 0.f), -180.f, 180.f, 1.f);
        setSliderFromFloat(mScaleSlider, editFloat(mScaleEdit, 1.f), 0.1f, 3.f, 0.01f);
        setSliderFromFloat(mZoomSlider, clampFinite(editFloat(mZoomEdit, 1.f), 1.f, 0.15f, 12.f), 0.1f, 12.f, 0.01f);
        mSyncingSliders = false;
    }

    void AssetStudioWindow::saveSession()
    {
        const std::string path = getenvText("OPENMW_FNV_ASSET_STUDIO_SAVE_FILE");
        if (path.empty())
        {
            setStatus("save path missing");
            Log(Debug::Warning) << "FNV/ESM4 asset studio save skipped reason=no-save-path"
                                << " gate=native-asset-studio-save runtime=known-blocked";
            return;
        }

        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file)
        {
            setStatus("save failed");
            Log(Debug::Warning) << "FNV/ESM4 asset studio save failed path=\"" << path << "\""
                                << " gate=native-asset-studio-save runtime=known-blocked";
            return;
        }

        file << "{\n";
        file << "  \"assetClass\": \"" << jsonEscape(editText(mAssetClassEdit)) << "\",\n";
        file << "  \"record\": \"" << jsonEscape(editText(mRecordEdit)) << "\",\n";
        file << "  \"session\": \"" << jsonEscape(editText(mSessionEdit)) << "\",\n";
        file << "  \"model\": \"" << jsonEscape(editText(mModelEdit)) << "\",\n";
        file << "  \"view\": \"" << jsonEscape(editText(mViewEdit)) << "\",\n";
        file << "  \"profile\": \"" << jsonEscape(editText(mProfileEdit)) << "\",\n";
        file << "  \"rotation\": { \"x\": " << editFloat(mRotXEdit, 0.f) << ", \"y\": " << editFloat(mRotYEdit, 0.f)
             << ", \"z\": " << editFloat(mRotZEdit, 0.f) << " },\n";
        file << "  \"scale\": " << editFloat(mScaleEdit, 1.f) << ",\n";
        file << "  \"zoom\": " << clampFinite(editFloat(mZoomEdit, 1.f), 1.f, 0.15f, 12.f) << ",\n";
        file << "  \"camera\": { \"panX\": " << editFloat(mPanXEdit, 0.f) << ", \"panZ\": "
             << editFloat(mPanZEdit, 0.f) << ", \"tiltDeg\": " << editFloat(mTiltEdit, 0.f) << " },\n";
        file << "  \"debug\": { \"axes\": " << (mAxesEnabled ? "true" : "false") << ", \"ik\": "
             << (mIkEnabled ? "true" : "false") << ", \"wireframe\": "
             << (mWireframeEnabled ? "true" : "false") << ", \"weights\": "
             << (mWeightsEnabled ? "true" : "false") << ", \"blood\": "
             << (mBloodVisible ? "true" : "false") << " }\n";
        file << "}\n";

        setStatus("saved " + path);
        Log(Debug::Info) << "FNV/ESM4 asset studio saved path=\"" << path << "\" assetClass=\""
                         << editText(mAssetClassEdit) << "\" record=\"" << editText(mRecordEdit)
                         << "\" gate=native-asset-studio-save runtime=runtime-supported";
    }

    void AssetStudioWindow::setStatus(const std::string& value)
    {
        if (mStatus != nullptr)
            mStatus->setCaption(value);
    }

    bool AssetStudioWindow::setStudioField(const std::string& field, const std::string& value)
    {
        const std::string lowered = lowerAscii(field);
        if (lowered == "assetclass" || lowered == "asset-class" || lowered == "class")
            mAssetClassEdit->setCaption(value);
        else if (lowered == "record" || lowered == "target" || lowered == "actortarget")
            mRecordEdit->setCaption(value);
        else if (lowered == "session")
            mSessionEdit->setCaption(value);
        else if (lowered == "model" || lowered == "modelpath")
            mModelEdit->setCaption(value);
        else if (lowered == "view")
            mViewEdit->setCaption(value);
        else if (lowered == "profile" || lowered == "actorprofile" || lowered == "part")
            mProfileEdit->setCaption(value);
        else if (lowered == "zoom")
        {
            char* end = nullptr;
            const float parsed = std::strtof(value.c_str(), &end);
            setEditFloat(mZoomEdit, clampFinite(end == value.c_str() ? 1.f : parsed, 1.f, 0.15f, 12.f));
        }
        else if (lowered == "panx" || lowered == "pan-x")
            mPanXEdit->setCaption(value);
        else if (lowered == "panz" || lowered == "pan-z")
            mPanZEdit->setCaption(value);
        else if (lowered == "tilt" || lowered == "tiltdeg" || lowered == "tilt-deg")
            mTiltEdit->setCaption(value);
        else if (lowered == "rotx" || lowered == "rot-x")
            mRotXEdit->setCaption(value);
        else if (lowered == "roty" || lowered == "rot-y")
            mRotYEdit->setCaption(value);
        else if (lowered == "rotz" || lowered == "rot-z")
            mRotZEdit->setCaption(value);
        else if (lowered == "scale")
            mScaleEdit->setCaption(value);
        else
            return false;

        return true;
    }

    void AssetStudioWindow::pollCommandFile(float duration)
    {
        const char* path = std::getenv("OPENMW_FNV_ASSET_STUDIO_COMMAND_FILE");
        if (path == nullptr || path[0] == '\0')
            return;

        mCommandPollElapsed += duration;
        if (mCommandPollElapsed < 0.05f)
            return;
        mCommandPollElapsed = 0.f;

        std::string content;
        if (!readWholeFile(path, content) || content.empty() || content == mLastCommandFileContent)
            return;

        std::string sequence;
        readJsonStringField(content, "sequence", sequence);
        if (!sequence.empty() && sequence == mLastCommandSequence)
            return;

        if (applyCommandFilePayload(content))
        {
            mLastCommandFileContent = content;
            mLastCommandSequence = sequence;
        }
    }

    bool AssetStudioWindow::applyCommandFilePayload(const std::string& content)
    {
        std::string command;
        if (!readJsonStringField(content, "command", command) || command.empty())
        {
            setStatus("cli command missing command");
            Log(Debug::Warning) << "FNV/ESM4 asset studio cli command rejected reason=missing-command"
                                << " gate=native-asset-studio-command-file runtime=known-blocked";
            return false;
        }

        std::string value;
        readJsonStringField(content, "value", value);
        std::string field;
        readJsonStringField(content, "field", field);
        std::string sequence;
        readJsonStringField(content, "sequence", sequence);

        const std::string lowered = lowerAscii(command);
        if (lowered == "load")
        {
            int changed = 0;
            bool hasAssetClass = false;
            bool hasRecord = false;
            bool hasModel = false;
            for (std::string_view candidate : { std::string_view("assetClass"), std::string_view("record"),
                     std::string_view("session"), std::string_view("model"), std::string_view("view"),
                     std::string_view("profile"), std::string_view("zoom"), std::string_view("panX"),
                     std::string_view("panZ"), std::string_view("tilt"), std::string_view("rotX"),
                     std::string_view("rotY"), std::string_view("rotZ"), std::string_view("scale") })
            {
                std::string fieldValue;
                if (readJsonStringField(content, candidate, fieldValue)
                    && setStudioField(std::string(candidate), fieldValue))
                {
                    ++changed;
                    if (candidate == "assetClass")
                        hasAssetClass = true;
                    else if (candidate == "record")
                        hasRecord = true;
                    else if (candidate == "model")
                        hasModel = true;
                }
            }
            if ((hasAssetClass || hasRecord) && !hasModel)
                mModelEdit->setCaption("");
            syncSlidersFromEdits();
            setStatus("cli load " + editText(mRecordEdit));
            Log(Debug::Info) << "FNV/ESM4 asset studio cli command sequence=\"" << sequence
                             << "\" command=\"load\" changed=" << changed
                             << " gate=native-asset-studio-command-file runtime=runtime-supported";
            rebuildPreview();
            return true;
        }

        if (lowered == "set")
        {
            if (field.empty() || !setStudioField(field, value))
            {
                setStatus("cli set rejected " + field);
                Log(Debug::Warning) << "FNV/ESM4 asset studio cli command rejected sequence=\"" << sequence
                                    << "\" command=\"set\" field=\""
                                    << field << "\" gate=native-asset-studio-command-file runtime=known-blocked";
                return false;
            }

            syncSlidersFromEdits();
            setStatus("cli set " + field);
            Log(Debug::Info) << "FNV/ESM4 asset studio cli command sequence=\"" << sequence
                             << "\" command=\"set\" field=\"" << field << "\" value=\"" << value
                             << "\" gate=native-asset-studio-command-file runtime=runtime-supported";
            if (lowerAscii(field) == "view" && promoteCameraTile(value))
                return true;
            if (isLiveTransformField(field))
                rebuildMainPreview();
            else
                rebuildPreview();
            return true;
        }

        applyStudioCommand(command, value);
        Log(Debug::Info) << "FNV/ESM4 asset studio cli command sequence=\"" << sequence
                         << "\" command=\"" << command << "\" value=\"" << value
                         << "\" gate=native-asset-studio-command-file runtime=runtime-supported";
        return true;
    }

    void AssetStudioWindow::updateToggleCaptions()
    {
        if (mAxesButton != nullptr)
            mAxesButton->setCaption(mAxesEnabled ? "Axes on" : "Axes off");
        if (mIkButton != nullptr)
            mIkButton->setCaption(mIkEnabled ? "IK on" : "IK off");
        if (mWireframeButton != nullptr)
            mWireframeButton->setCaption(mWireframeEnabled ? "Wire on" : "Wire off");
        if (mWeightsButton != nullptr)
            mWeightsButton->setCaption(mWeightsEnabled ? "Wgt on" : "Wgt off");
        if (mBloodButton != nullptr)
            mBloodButton->setCaption(mBloodVisible ? "Blood on" : "Blood off");
    }

    bool AssetStudioWindow::isActorAssetClass(const std::string& assetClass) const
    {
        const std::string lowered = lowerAscii(assetClass);
        return lowered == "npc" || lowered == "person" || lowered == "actor" || lowered == "character"
            || lowered == "creature";
    }

    void AssetStudioWindow::populateAssetCategories()
    {
        mAssetCategoryCombo->removeAllItems();
        mAssetCategoryList->removeAllItems();
        mAssetCategoryClasses.clear();

        const std::array<std::pair<std::string_view, std::string_view>, 17> categories{ {
            { "All", "all" },
            { "Animated", "animated" },
            { "NPC", "npc" },
            { "Creature", "creature" },
            { "Door", "door" },
            { "Activator", "acti" },
            { "Container", "cont" },
            { "Furniture", "furn" },
            { "Weapon", "weap" },
            { "Armor", "armo" },
            { "Static", "stat" },
            { "Misc", "misc" },
            { "Light", "ligh" },
            { "Flora", "flor" },
            { "Ingredient", "ingr" },
            { "Book", "book" },
            { "Ammo", "ammo" },
        } };

        std::size_t selected = 0;
        const std::string currentClass = lowerAscii(editText(mAssetClassEdit));
        for (std::size_t i = 0; i < categories.size(); ++i)
        {
            mAssetCategoryCombo->addItem(std::string(categories[i].first), std::string(categories[i].second));
            mAssetCategoryList->addItem(std::string(categories[i].first), std::string(categories[i].second));
            mAssetCategoryClasses.push_back(std::string(categories[i].second));
            if (currentClass == categories[i].second)
                selected = i;
        }
        mSelectedAssetCategory = selected < mAssetCategoryClasses.size() ? mAssetCategoryClasses[selected] : "all";
        mAssetCategoryCombo->setIndexSelected(selected);
        mAssetCategoryCombo->setCaptionWithReplacing(mAssetCategoryCombo->getItemNameAt(selected));
        mAssetCategoryList->setIndexSelected(selected);
    }

    std::string AssetStudioWindow::selectedAssetClass() const
    {
        return mSelectedAssetCategory.empty() ? "all" : mSelectedAssetCategory;
    }

    void AssetStudioWindow::updateAssetResults()
    {
        if (mUpdatingAssetResults)
            return;
        mUpdatingAssetResults = true;
        mAssetResultList->removeAllItems();

        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr)
        {
            mAssetCountText->setCaption("store unavailable");
            mUpdatingAssetResults = false;
            return;
        }

        const std::string category = selectedAssetClass();
        const std::string filter = lowerAscii(editText(mAssetSearchEdit));
        std::vector<CatalogItem> items;
        int scanned = 0;

        const bool all = category == "all";
        const bool animated = category == "animated";
        auto wants = [&](std::string_view assetClass) {
            if (all)
                return true;
            if (animated)
                return assetClass == "npc" || assetClass == "creature" || assetClass == "door" || assetClass == "acti"
                    || assetClass == "cont" || assetClass == "furn" || assetClass == "weap" || assetClass == "armo";
            return category == assetClass;
        };

        if (wants("npc"))
        {
            for (const ESM4::Npc& candidate : store->get<ESM4::Npc>())
            {
                ++scanned;
                if (!candidate.mIsFONV)
                    continue;
                const std::string formId = ESM::RefId(candidate.mId).toDebugString();
                if (!catalogFilterMatches(filter, { "npc", candidate.mFullName, candidate.mEditorId, formId, candidate.mModel }))
                    continue;
                const std::string record = firstNonEmpty({ candidate.mEditorId, candidate.mFullName, formId });
                items.push_back(CatalogItem{ "npc", record,
                    catalogText("NPC", candidate.mFullName, candidate.mEditorId, formId, candidate.mModel),
                    candidate.mModel });
            }
        }
        if (wants("creature"))
            appendRecordCatalog<ESM4::Creature>(items, *store, "creature", filter, scanned,
                [](const ESM4::Creature& value) { return value.mModel; });
        if (wants("door"))
            appendRecordCatalog<ESM4::Door>(
                items, *store, "door", filter, scanned, [](const ESM4::Door& value) { return value.mModel; });
        if (wants("acti"))
            appendRecordCatalog<ESM4::Activator>(items, *store, "acti", filter, scanned,
                [](const ESM4::Activator& value) { return value.mModel; });
        if (wants("cont"))
            appendRecordCatalog<ESM4::Container>(items, *store, "cont", filter, scanned,
                [](const ESM4::Container& value) { return value.mModel; });
        if (wants("furn"))
            appendRecordCatalog<ESM4::Furniture>(items, *store, "furn", filter, scanned,
                [](const ESM4::Furniture& value) { return value.mModel; });
        if (wants("weap"))
            appendRecordCatalog<ESM4::Weapon>(
                items, *store, "weap", filter, scanned, [](const ESM4::Weapon& value) { return value.mModel; });
        if (wants("armo"))
            appendRecordCatalog<ESM4::Armor>(items, *store, "armo", filter, scanned, [](const ESM4::Armor& value) {
                return firstNonEmpty(
                    { value.mModelMaleWorld, value.mModelFemaleWorld, value.mModelMale, value.mModelFemale, value.mModel });
            });
        if (wants("stat"))
            appendRecordCatalog<ESM4::Static>(
                items, *store, "stat", filter, scanned, [](const ESM4::Static& value) { return value.mModel; });
        if (wants("misc"))
            appendRecordCatalog<ESM4::MiscItem>(
                items, *store, "misc", filter, scanned, [](const ESM4::MiscItem& value) { return value.mModel; });
        if (wants("ligh"))
            appendRecordCatalog<ESM4::Light>(
                items, *store, "ligh", filter, scanned, [](const ESM4::Light& value) { return value.mModel; });
        if (wants("flor"))
            appendRecordCatalog<ESM4::Flora>(
                items, *store, "flor", filter, scanned, [](const ESM4::Flora& value) { return value.mModel; });
        if (wants("ingr"))
            appendRecordCatalog<ESM4::Ingredient>(items, *store, "ingr", filter, scanned,
                [](const ESM4::Ingredient& value) { return value.mModel; });
        if (wants("book"))
            appendRecordCatalog<ESM4::Book>(
                items, *store, "book", filter, scanned, [](const ESM4::Book& value) { return value.mModel; });
        if (wants("ammo"))
            appendRecordCatalog<ESM4::Ammunition>(items, *store, "ammo", filter, scanned,
                [](const ESM4::Ammunition& value) { return value.mModel; });
        if (wants("alch"))
            appendRecordCatalog<ESM4::Potion>(
                items, *store, "alch", filter, scanned, [](const ESM4::Potion& value) { return value.mModel; });

        std::sort(items.begin(), items.end(), [](const CatalogItem& left, const CatalogItem& right) {
            return lowerAscii(left.mLabel) < lowerAscii(right.mLabel);
        });

        constexpr std::size_t maxVisible = 250;
        const std::size_t visible = std::min(items.size(), maxVisible);
        for (std::size_t i = 0; i < visible; ++i)
            mAssetResultList->addItem(items[i].mLabel, catalogData(items[i].mAssetClass, items[i].mRecord, items[i].mModel));

        mAssetCountText->setCaption(std::to_string(visible) + "/" + std::to_string(items.size()) + " shown");
        if (visible > 0)
            mAssetResultList->setIndexSelected(0);
        mUpdatingAssetResults = false;
        Log(Debug::Info) << "FNV/ESM4 asset studio catalog category=\"" << category << "\" filter=\""
                         << editText(mAssetSearchEdit) << "\" scanned=" << scanned << " matched=" << items.size()
                         << " visible=" << visible
                         << " gate=native-asset-studio-catalog runtime=runtime-supported";
    }

    void AssetStudioWindow::applySelectedAsset(bool rebuild)
    {
        if (mAssetResultList == nullptr || mAssetResultList->getIndexSelected() == MyGUI::ITEM_NONE)
        {
            if (mAssetResultList != nullptr && mAssetResultList->getItemCount() > 0)
                mAssetResultList->setIndexSelected(0);
        }

        if (mAssetResultList == nullptr || mAssetResultList->getIndexSelected() == MyGUI::ITEM_NONE)
        {
            setStatus("select an asset first");
            Log(Debug::Warning) << "FNV/ESM4 asset studio picker load skipped reason=no-selection"
                                << " gate=native-asset-studio-picker runtime=known-blocked";
            return;
        }

        std::string assetClass;
        std::string record;
        std::string model;
        const std::string data = *mAssetResultList->getItemDataAt<std::string>(mAssetResultList->getIndexSelected());
        if (!splitCatalogData(data, assetClass, record, model))
            return;

        mAssetClassEdit->setCaption(assetClass);
        mRecordEdit->setCaption(record);
        mModelEdit->setCaption(model);
        if (assetClass == "npc")
            mProfileEdit->setCaption("face");
        else if (assetClass == "creature")
            mProfileEdit->setCaption("full-body");
        else if (!isActorAssetClass(assetClass))
            mProfileEdit->setCaption("");

        setStatus("selected " + record);
        Log(Debug::Info) << "FNV/ESM4 asset studio picker selected assetClass=\"" << assetClass
                         << "\" record=\"" << record << "\" model=\"" << model << "\" rebuild=" << rebuild
                         << " gate=native-asset-studio-picker runtime=runtime-supported";
        if (rebuild)
            rebuildPreview();
    }

    void AssetStudioWindow::requestScreenshot()
    {
        const std::string path = getenvText("OPENMW_FNV_LIVE_RUNTIME_COMMAND_FILE");
        if (path.empty())
        {
            setStatus("screenshot path missing");
            Log(Debug::Warning) << "FNV/ESM4 asset studio screenshot skipped reason=no-live-runtime-command-file"
                                << " gate=native-asset-studio-screenshot runtime=known-blocked";
            return;
        }

        static unsigned int screenshotCounter = 0;
        const std::string requestId
            = "asset-studio-" + std::to_string(std::time(nullptr)) + "-" + std::to_string(++screenshotCounter);
        const std::string label = editText(mAssetClassEdit) + "-" + editText(mRecordEdit) + "-" + editText(mViewEdit);
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file)
        {
            setStatus("screenshot write failed");
            Log(Debug::Warning) << "FNV/ESM4 asset studio screenshot failed path=\"" << path << "\""
                                << " gate=native-asset-studio-screenshot runtime=known-blocked";
            return;
        }

        file << "{\n";
        file << "  \"schema\": \"nikami-fnv-live-runtime-command-v1\",\n";
        file << "  \"command\": \"request-screenshot\",\n";
        file << "  \"screenshotRequestId\": \"" << jsonEscape(requestId) << "\",\n";
        file << "  \"screenshotLabel\": \"" << jsonEscape(label) << "\",\n";
        file << "  \"actorTarget\": \"" << jsonEscape(editText(mRecordEdit)) << "\",\n";
        file << "  \"assetClass\": \"" << jsonEscape(editText(mAssetClassEdit)) << "\"\n";
        file << "}\n";

        setStatus("screenshot queued");
        Log(Debug::Info) << "FNV/ESM4 asset studio screenshot requested id=\"" << requestId << "\" label=\""
                         << label << "\" path=\"" << path
                         << "\" gate=native-asset-studio-screenshot runtime=runtime-supported";
    }

    void AssetStudioWindow::retireCurrentPreviews()
    {
        mRetiredPreview = std::move(mPreview);
        mRetiredPreviewTexture = std::move(mPreviewTexture);
        mRetiredCameraPreviews = std::move(mCameraPreviews);
        mRetiredCameraPreviewTextures = std::move(mCameraPreviewTextures);
        mRetiredActorNpcProxy = std::move(mActorNpcProxy);
        mRetiredActorCreatureProxy = std::move(mActorCreatureProxy);
        mRetiredActorPreview = std::move(mActorPreview);
        mRetiredActorPreviewTexture = std::move(mActorPreviewTexture);
        mRetiredActorCameraPreviews = std::move(mActorCameraPreviews);
        mRetiredActorCameraPreviewTextures = std::move(mActorCameraPreviewTextures);
        mRetiredPreviewSeconds = 0.35f;
    }

    void AssetStudioWindow::clearRetiredPreviews()
    {
        mRetiredPreview.reset();
        mRetiredPreviewTexture.reset();
        for (std::size_t i = 0; i < mRetiredCameraPreviews.size(); ++i)
        {
            mRetiredCameraPreviews[i].reset();
            mRetiredCameraPreviewTextures[i].reset();
            mRetiredActorCameraPreviews[i].reset();
            mRetiredActorCameraPreviewTextures[i].reset();
        }
        mRetiredActorPreview.reset();
        mRetiredActorPreviewTexture.reset();
        mRetiredActorNpcProxy.reset();
        mRetiredActorCreatureProxy.reset();
    }

    void AssetStudioWindow::clearPendingPreviews()
    {
        mPendingPreviewSeconds = 0.f;
        mPendingPreviewFrames = 0;
        mPendingCameraTiles = false;
        mPendingPreview.reset();
        mPendingPreviewTexture.reset();
        for (std::size_t i = 0; i < mPendingCameraPreviews.size(); ++i)
        {
            mPendingCameraPreviews[i].reset();
            mPendingCameraPreviewTextures[i].reset();
            mPendingActorCameraPreviews[i].reset();
            mPendingActorCameraPreviewTextures[i].reset();
        }
        mPendingActorPreview.reset();
        mPendingActorPreviewTexture.reset();
        mPendingActorNpcProxy.reset();
        mPendingActorCreatureProxy.reset();
    }

    void AssetStudioWindow::promotePendingPreviews()
    {
        if (mPendingPreview == nullptr && mPendingActorPreview == nullptr)
            return;

        retireCurrentPreviews();
        if (mPendingActorPreview != nullptr)
        {
            attachTexture(mPreviewImage, mPendingActorPreviewTexture.get());
            mActorNpcProxy = std::move(mPendingActorNpcProxy);
            mActorCreatureProxy = std::move(mPendingActorCreatureProxy);
            mActorPreview = std::move(mPendingActorPreview);
            mActorPreviewTexture = std::move(mPendingActorPreviewTexture);
            if (mPendingCameraTiles)
            {
                for (std::size_t i = 0; i < mPendingActorCameraPreviews.size(); ++i)
                {
                    if (mPendingActorCameraPreviewTextures[i] != nullptr)
                    {
                        attachTexture(mCameraImages[i], mPendingActorCameraPreviewTextures[i].get());
                        mActorCameraPreviews[i] = std::move(mPendingActorCameraPreviews[i]);
                        mActorCameraPreviewTextures[i] = std::move(mPendingActorCameraPreviewTextures[i]);
                    }
                }
            }
        }
        else
        {
            attachTexture(mPreviewImage, mPendingPreviewTexture.get());
            mPreview = std::move(mPendingPreview);
            mPreviewTexture = std::move(mPendingPreviewTexture);
            if (mPendingCameraTiles)
            {
                for (std::size_t i = 0; i < mPendingCameraPreviews.size(); ++i)
                {
                    if (mPendingCameraPreviewTextures[i] != nullptr)
                    {
                        attachTexture(mCameraImages[i], mPendingCameraPreviewTextures[i].get());
                        mCameraPreviews[i] = std::move(mPendingCameraPreviews[i]);
                        mCameraPreviewTextures[i] = std::move(mPendingCameraPreviewTextures[i]);
                    }
                }
            }
        }

        mPendingPreviewSeconds = 0.f;
        mPendingPreviewFrames = 0;
        mPendingCameraTiles = false;
        Log(Debug::Info) << "FNV/ESM4 asset studio preview promoted gate=native-asset-studio-preview-swap"
                         << " runtime=runtime-supported";
    }

    void AssetStudioWindow::onApply(MyGUI::Widget*)
    {
        applyStudioCommand("apply");
    }

    void AssetStudioWindow::onAssetCategoryChanged(MyGUI::ComboBox* sender, size_t index)
    {
        if (index != MyGUI::ITEM_NONE)
        {
            sender->setCaptionWithReplacing(sender->getItemNameAt(index));
            if (index < mAssetCategoryClasses.size())
            {
                mSelectedAssetCategory = mAssetCategoryClasses[index];
                mAssetCategoryList->setIndexSelected(index);
            }
        }
        Log(Debug::Info) << "FNV/ESM4 asset studio picker category-click index=" << index << " caption=\""
                         << (index != MyGUI::ITEM_NONE ? sender->getItemNameAt(index).asUTF8() : std::string("none"))
                         << "\" selectedCategory=\"" << selectedAssetClass()
                         << "\" gate=native-asset-studio-picker runtime=runtime-supported";
        updateAssetResults();
    }

    void AssetStudioWindow::onAssetCategoryListChanged(MyGUI::ListBox* sender, size_t index)
    {
        if (index == MyGUI::ITEM_NONE || mUpdatingAssetResults)
            return;
        if (index < mAssetCategoryClasses.size())
        {
            mSelectedAssetCategory = mAssetCategoryClasses[index];
            mAssetCategoryCombo->setIndexSelected(index);
            mAssetCategoryCombo->setCaptionWithReplacing(sender->getItemNameAt(index));
        }
        Log(Debug::Info) << "FNV/ESM4 asset studio picker category-list-click index=" << index << " caption=\""
                         << sender->getItemNameAt(index) << "\" selectedCategory=\"" << selectedAssetClass()
                         << "\" gate=native-asset-studio-picker runtime=runtime-supported";
        updateAssetResults();
    }

    void AssetStudioWindow::onAssetSearchChanged(MyGUI::EditBox*)
    {
        Log(Debug::Info) << "FNV/ESM4 asset studio picker search-click filter=\"" << editText(mAssetSearchEdit)
                         << "\" gate=native-asset-studio-picker runtime=runtime-supported";
        updateAssetResults();
    }

    void AssetStudioWindow::onAssetSelected(MyGUI::ListBox*, size_t index)
    {
        if (index == MyGUI::ITEM_NONE || mUpdatingAssetResults)
            return;
        Log(Debug::Info) << "FNV/ESM4 asset studio picker list-click index=" << index
                         << " gate=native-asset-studio-picker runtime=runtime-supported";
        applySelectedAsset(false);
    }

    void AssetStudioWindow::onAssetAccepted(MyGUI::ListBox*, size_t index)
    {
        if (index == MyGUI::ITEM_NONE)
            return;
        Log(Debug::Info) << "FNV/ESM4 asset studio picker list-accept index=" << index
                         << " gate=native-asset-studio-picker runtime=runtime-supported";
        applySelectedAsset(true);
    }

    void AssetStudioWindow::onAssetLoad(MyGUI::Widget*)
    {
        Log(Debug::Info) << "FNV/ESM4 asset studio picker load-click"
                         << " gate=native-asset-studio-picker runtime=runtime-supported";
        applySelectedAsset(true);
    }

    void AssetStudioWindow::onScreenshot(MyGUI::Widget*)
    {
        Log(Debug::Info) << "FNV/ESM4 asset studio picker screenshot-click"
                         << " gate=native-asset-studio-picker runtime=runtime-supported";
        requestScreenshot();
    }

    void AssetStudioWindow::onSave(MyGUI::Widget*)
    {
        applyStudioCommand("save");
    }

    void AssetStudioWindow::onSnap(MyGUI::Widget* sender)
    {
        if (sender == mSnapXNegButton)
            applyStudioCommand("snap-x", "-90");
        else if (sender == mSnapXZeroButton)
            applyStudioCommand("reset-rotation");
        else if (sender == mSnapXPosButton)
            applyStudioCommand("snap-x", "90");
        else if (sender == mSnapYNegButton)
            applyStudioCommand("snap-y", "-90");
        else if (sender == mSnapYPosButton)
            applyStudioCommand("snap-y", "90");
        else if (sender == mSnapZPosButton)
            applyStudioCommand("snap-z", "90");
    }

    void AssetStudioWindow::onViewButton(MyGUI::Widget* sender)
    {
        if (sender == mViewFrontButton)
            applyStudioCommand("view", "front");
        else if (sender == mViewLeftButton)
            applyStudioCommand("view", "left");
        else if (sender == mViewTopButton)
            applyStudioCommand("view", "top");
        else
        {
            for (std::size_t i = 0; i < mCameraImages.size(); ++i)
            {
                if (sender == mCameraImages[i] || sender == mCameraHotspots[i])
                {
                    applyStudioCommand("view", cameraTileView(i));
                    return;
                }
            }
        }
    }

    void AssetStudioWindow::onProfileButton(MyGUI::Widget* sender)
    {
        if (sender == mProfileFaceButton)
            applyStudioCommand("profile", "face");
        else if (sender == mProfileBodyButton)
            applyStudioCommand("profile", "full-body");
        else if (sender == mProfileHandsButton)
            applyStudioCommand("profile", "hands-close");
        else if (sender == mProfileWeaponButton)
            applyStudioCommand("profile", "weapon-arms");
    }

    void AssetStudioWindow::onDebugButton(MyGUI::Widget* sender)
    {
        if (sender == mAxesButton)
            applyStudioCommand("toggle-axes");
        else if (sender == mIkButton)
            applyStudioCommand("toggle-ik");
        else if (sender == mWireframeButton)
            applyStudioCommand("toggle-wireframe");
        else if (sender == mWeightsButton)
            applyStudioCommand("toggle-weights");
        else if (sender == mBloodButton)
            applyStudioCommand("toggle-blood");
        else if (sender == mInventoryButton)
        {
            setStatus("inventory: use original OpenMW inventory path");
            Log(Debug::Info) << "FNV/ESM4 asset studio inventory button ignored reason=not-wired-to-original-inventory"
                             << " gate=native-asset-studio-inventory runtime=loaded-pending-runtime";
        }
        else if (sender == mEquipmentButton)
        {
            setStatus("equip: use original OpenMW paper doll path");
            Log(Debug::Info) << "FNV/ESM4 asset studio equipment button ignored reason=not-wired-to-original-paper-doll"
                             << " gate=native-asset-studio-equipment runtime=loaded-pending-runtime";
        }
    }

    void AssetStudioWindow::onAccept(MyGUI::EditBox* sender)
    {
        syncSlidersFromEdits();
        if (sender == mZoomEdit || sender == mPanXEdit || sender == mPanZEdit || sender == mTiltEdit
            || sender == mRotXEdit || sender == mRotYEdit || sender == mRotZEdit || sender == mScaleEdit)
            rebuildMainPreview();
        else
            rebuildPreview();
    }

    void AssetStudioWindow::applyStudioCommand(const std::string& command, const std::string& value)
    {
        bool rebuild = true;
        bool mainOnlyRebuild = false;
        if (command == "apply")
            syncSlidersFromEdits();
        else if (command == "save")
        {
            saveSession();
            rebuild = false;
        }
        else if (command == "view")
        {
            mViewEdit->setCaption(value);
            rebuild = !promoteCameraTile(value);
        }
        else if (command == "profile")
            mProfileEdit->setCaption(value);
        else if (command == "snap-x")
        {
            setEditFloat(mRotXEdit, std::strtof(value.c_str(), nullptr));
            mainOnlyRebuild = true;
        }
        else if (command == "snap-y")
        {
            setEditFloat(mRotYEdit, std::strtof(value.c_str(), nullptr));
            mainOnlyRebuild = true;
        }
        else if (command == "snap-z")
        {
            setEditFloat(mRotZEdit, std::strtof(value.c_str(), nullptr));
            mainOnlyRebuild = true;
        }
        else if (command == "reset-rotation")
        {
            setEditFloat(mRotXEdit, 0.f);
            setEditFloat(mRotYEdit, 0.f);
            setEditFloat(mRotZEdit, 0.f);
            syncSlidersFromEdits();
            mainOnlyRebuild = true;
        }
        else if (command == "axes-on" || command == "axes-off")
        {
            mAxesEnabled = command == "axes-on";
            setProcessEnv("OPENMW_FNV_DRAW_PART_AXES", mAxesEnabled);
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "toggle-axes")
        {
            mAxesEnabled = !mAxesEnabled;
            setProcessEnv("OPENMW_FNV_DRAW_PART_AXES", mAxesEnabled);
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "ik-on" || command == "ik-off")
        {
            mIkEnabled = command == "ik-on";
            setProcessEnv("OPENMW_FNV_SHOW_IK_BONES", mIkEnabled);
            setProcessEnv("OPENMW_FNV_BONE_IK_DEBUG", false);
            setProcessEnv("OPENMW_FNV_DISABLE_WEAPON_IK", true);
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "toggle-ik")
        {
            mIkEnabled = !mIkEnabled;
            setProcessEnv("OPENMW_FNV_SHOW_IK_BONES", mIkEnabled);
            setProcessEnv("OPENMW_FNV_BONE_IK_DEBUG", false);
            setProcessEnv("OPENMW_FNV_DISABLE_WEAPON_IK", true);
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "wireframe-on" || command == "wireframe-off")
        {
            mWireframeEnabled = command == "wireframe-on";
            setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME", mWireframeEnabled);
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "toggle-wireframe")
        {
            mWireframeEnabled = !mWireframeEnabled;
            setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_WIREFRAME", mWireframeEnabled);
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "weights-on" || command == "weights-off")
        {
            mWeightsEnabled = command == "weights-on";
            setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS", mWeightsEnabled);
            setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS", mWeightsEnabled);
            setProcessEnvText("OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE", mWeightsEnabled ? "all" : "");
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "toggle-weights")
        {
            mWeightsEnabled = !mWeightsEnabled;
            setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_SKIN_WEIGHTS", mWeightsEnabled);
            setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_FINGER_WEIGHTS", mWeightsEnabled);
            setProcessEnvText("OPENMW_FNV_ACTOR_PREVIEW_WEIGHT_BONE", mWeightsEnabled ? "all" : "");
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "blood-on" || command == "blood-off")
        {
            mBloodVisible = command == "blood-on";
            setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_HIDE_BLOOD", !mBloodVisible);
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else if (command == "toggle-blood")
        {
            mBloodVisible = !mBloodVisible;
            setProcessEnv("OPENMW_FNV_ACTOR_PREVIEW_HIDE_BLOOD", !mBloodVisible);
            updateToggleCaptions();
            mainOnlyRebuild = true;
        }
        else
        {
            setStatus("unknown command " + command);
            Log(Debug::Warning) << "FNV/ESM4 asset studio command rejected command=\"" << command << "\" value=\""
                                << value << "\" gate=native-asset-studio-command runtime=known-blocked";
            return;
        }

        if (command.rfind("snap-", 0) == 0)
            syncSlidersFromEdits();

        if (command == "view" && !rebuild)
            setStatus("view " + value + " instant");
        else
            setStatus(value.empty() ? ("button " + command) : ("button " + command + " " + value));
        Log(Debug::Info) << "FNV/ESM4 asset studio command command=\"" << command << "\" value=\"" << value
                         << "\" axes=" << mAxesEnabled << " ik=" << mIkEnabled
                         << " wireframe=" << mWireframeEnabled << " weights=" << mWeightsEnabled
                         << " blood=" << mBloodVisible
                         << " gate=native-asset-studio-command runtime=runtime-supported";

        if (mainOnlyRebuild)
            rebuildMainPreview();
        else if (rebuild)
            rebuildPreview();
    }

    void AssetStudioWindow::onSliderMoved(MyGUI::ScrollBar* sender, size_t)
    {
        if (mSyncingSliders)
            return;

        std::string sliderName;
        if (sender == mRotXSlider)
        {
            sliderName = "rot-x";
            setEditFloat(mRotXEdit, sliderFloat(sender, -180.f, 1.f));
        }
        else if (sender == mRotYSlider)
        {
            sliderName = "rot-y";
            setEditFloat(mRotYEdit, sliderFloat(sender, -180.f, 1.f));
        }
        else if (sender == mRotZSlider)
        {
            sliderName = "rot-z";
            setEditFloat(mRotZEdit, sliderFloat(sender, -180.f, 1.f));
        }
        else if (sender == mScaleSlider)
        {
            sliderName = "scale";
            setEditFloat(mScaleEdit, sliderFloat(sender, 0.1f, 0.01f));
        }
        else if (sender == mZoomSlider)
        {
            sliderName = "zoom";
            setEditFloat(mZoomEdit, clampFinite(sliderFloat(sender, 0.1f, 0.01f), 1.f, 0.15f, 12.f));
        }

        setStatus("slider " + sliderName);
        Log(Debug::Info) << "FNV/ESM4 asset studio slider slider=\"" << sliderName << "\" rotation=("
                         << editFloat(mRotXEdit, 0.f) << "," << editFloat(mRotYEdit, 0.f) << ","
                         << editFloat(mRotZEdit, 0.f) << ") scale=" << editFloat(mScaleEdit, 1.f)
                         << " zoom=" << clampFinite(editFloat(mZoomEdit, 1.f), 1.f, 0.15f, 12.f)
                         << " gate=native-asset-studio-command runtime=runtime-supported";
        rebuildMainPreview();
    }

    void AssetStudioWindow::rebuildPreview()
    {
        const std::string assetClass = editText(mAssetClassEdit).empty() ? "mesh" : editText(mAssetClassEdit);
        const std::string record = editText(mRecordEdit).empty() ? "direct-model" : editText(mRecordEdit);
        const std::string session = editText(mSessionEdit).empty() ? "native-session" : editText(mSessionEdit);
        const std::string view = editText(mViewEdit).empty() ? "front" : editText(mViewEdit);
        const std::string profile = editText(mProfileEdit);
        std::string model = editText(mModelEdit);
        const float zoom = clampFinite(editFloat(mZoomEdit, 1.f), 1.f, 0.15f, 12.f);
        setEditFloat(mZoomEdit, zoom);

        Log(Debug::Info) << "FNV/ESM4 asset studio selector assetClass=\"" << assetClass << "\" record=\"" << record
                         << "\" session=\"" << session << "\" model=\"" << model << "\" view=\"" << view
                         << "\" profile=\"" << profile
                         << "\" zoom=" << zoom
                         << " gate=native-asset-studio-selector runtime=loaded-pending-runtime";

        if (isActorAssetClass(assetClass))
        {
            rebuildActorPreview(assetClass, record, session, view, profile, zoom, true);
            return;
        }

        if (model.empty())
        {
            model = resolveRecordModel(assetClass, record);
            if (!model.empty())
                mModelEdit->setCaption(model);
        }

        rebuildModelPreview(assetClass, record, session, view, model, zoom, true);
    }

    void AssetStudioWindow::rebuildMainPreview()
    {
        const std::string assetClass = editText(mAssetClassEdit).empty() ? "mesh" : editText(mAssetClassEdit);
        const std::string record = editText(mRecordEdit).empty() ? "direct-model" : editText(mRecordEdit);
        const std::string session = editText(mSessionEdit).empty() ? "native-session" : editText(mSessionEdit);
        const std::string view = editText(mViewEdit).empty() ? "front" : editText(mViewEdit);
        const std::string profile = editText(mProfileEdit);
        std::string model = editText(mModelEdit);
        const float zoom = clampFinite(editFloat(mZoomEdit, 1.f), 1.f, 0.15f, 12.f);
        setEditFloat(mZoomEdit, zoom);

        if (isActorAssetClass(assetClass))
        {
            rebuildActorPreview(assetClass, record, session, view, profile, zoom, false);
            return;
        }

        if (model.empty())
        {
            model = resolveRecordModel(assetClass, record);
            if (!model.empty())
                mModelEdit->setCaption(model);
        }

        rebuildModelPreview(assetClass, record, session, view, model, zoom, false);
    }

    bool AssetStudioWindow::rebuildActorPreview(const std::string& assetClass, const std::string& record,
        const std::string& session, const std::string& view, const std::string& profile, float zoom,
        bool rebuildCameraTiles)
    {
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

            std::unique_ptr<MWWorld::LiveCellRef<ESM4::Npc>> actorNpcProxy;
            std::unique_ptr<MWWorld::LiveCellRef<ESM4::Creature>> actorCreatureProxy;
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
                actorNpcProxy = std::make_unique<MWWorld::LiveCellRef<ESM4::Npc>>(proxyRef, npc);
                actor = MWWorld::Ptr(actorNpcProxy.get(), nullptr);
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
                actorCreatureProxy = std::make_unique<MWWorld::LiveCellRef<ESM4::Creature>>(proxyRef, creature);
                actor = MWWorld::Ptr(actorCreatureProxy.get(), nullptr);
                actorId = creature->mEditorId;
                fullName = creature->mFullName;
                formId = ESM::RefId(creature->mId).toDebugString();
                resolvedKind = "creature";
            }
            else
            {
                throw std::runtime_error("could not resolve actor record " + record);
            }

            const osg::Vec3f editorRotation(
                editFloat(mRotXEdit, 0.f), editFloat(mRotYEdit, 0.f), editFloat(mRotZEdit, 0.f));
            const float editorScale = editFloat(mScaleEdit, 1.f);
            setProcessEnvText("OPENMW_FNV_ASSET_STUDIO_CAMERA_PAN_X", editText(mPanXEdit));
            setProcessEnvText("OPENMW_FNV_ASSET_STUDIO_CAMERA_PAN_Z", editText(mPanZEdit));
            setProcessEnvText("OPENMW_FNV_ASSET_STUDIO_CAMERA_TILT_DEG", editText(mTiltEdit));

            auto actorPreview = std::make_unique<MWRender::FalloutActorPreview>(
                mParent, mResourceSystem, actor, actorViewModeForView(view), zoom, profile, editorRotation, editorScale);
            actorPreview->rebuild();
            actorPreview->redraw();
            auto actorPreviewTexture = std::make_unique<MyGUIPlatform::OSGTexture>(
                actorPreview->getTexture().get(), actorPreview->getTextureStateSet());
            clearPendingPreviews();
            mPendingActorNpcProxy = std::move(actorNpcProxy);
            mPendingActorCreatureProxy = std::move(actorCreatureProxy);
            mPendingActorPreview = std::move(actorPreview);
            mPendingActorPreviewTexture = std::move(actorPreviewTexture);

            if (rebuildCameraTiles)
            {
                std::array<std::unique_ptr<MWRender::FalloutActorPreview>, 8> actorCameraPreviews;
                std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 8> actorCameraPreviewTextures;
                for (std::size_t i = 0; i < sCameraTileViews.size(); ++i)
                {
                    actorCameraPreviews[i]
                        = std::make_unique<MWRender::FalloutActorPreview>(
                            mParent, mResourceSystem, actor, actorViewModeForView(cameraTileView(i)), zoom, profile,
                            editorRotation, editorScale);
                    actorCameraPreviews[i]->rebuild();
                    actorCameraPreviews[i]->redraw();
                    actorCameraPreviewTextures[i] = std::make_unique<MyGUIPlatform::OSGTexture>(
                        actorCameraPreviews[i]->getTexture().get(), actorCameraPreviews[i]->getTextureStateSet());
                }
                for (std::size_t i = 0; i < sCameraTileViews.size(); ++i)
                {
                    mPendingActorCameraPreviews[i] = std::move(actorCameraPreviews[i]);
                    mPendingActorCameraPreviewTextures[i] = std::move(actorCameraPreviewTextures[i]);
                }
            }
            mPendingCameraTiles = rebuildCameraTiles;
            mPendingPreviewSeconds = 0.2f;
            mPendingPreviewFrames = 3;

            setStatus("actor loaded " + actorId);
            Log(Debug::Info) << "FNV/ESM4 asset studio actor loaded assetClass=\"" << assetClass << "\" record=\""
                             << record << "\" resolvedKind=\"" << resolvedKind << "\" actor=\"" << actorId
                             << "\" full=\"" << fullName << "\" form=" << formId << " session=\"" << session
                             << "\" view=\"" << view << "\" profile=\"" << profile << "\" zoom=" << zoom
                             << " cameraPan=(" << editFloat(mPanXEdit, 0.f) << ","
                             << editFloat(mPanZEdit, 0.f) << ") cameraTiltDeg=" << editFloat(mTiltEdit, 0.f)
                             << " rotation=(" << editorRotation.x() << "," << editorRotation.y() << ","
                             << editorRotation.z() << ") scale=" << editorScale
                             << " panes=4 threeCamera=1 cameraTileRebuild=" << rebuildCameraTiles
                             << " scannedNpcBases=" << scannedNpcs << " scannedCreatureBases=" << scannedCreatures
                             << " gate=native-asset-studio-actor-preview runtime=runtime-supported";
            if (rebuildCameraTiles)
                Log(Debug::Info) << "FNV/ESM4 asset studio three camera actor preview assetClass=\"" << assetClass
                                 << "\" record=\"" << record << "\" actor=\"" << actorId
                                 << "\" views=\"front,back,left,right,top,bottom,iso-nw,iso-sw\" profile=\"" << profile
                                 << "\" zoom=" << zoom
                                 << " gate=native-asset-studio-three-camera-actor-preview runtime=runtime-supported";
            return true;
        }
        catch (const std::exception& e)
        {
            if (mActorPreview == nullptr && mPreview == nullptr)
                mPreviewImage->setRenderItemTexture(nullptr);
            if (rebuildCameraTiles && mActorPreview == nullptr && mPreview == nullptr)
            {
                for (std::size_t i = 0; i < mActorCameraPreviews.size(); ++i)
                {
                    mActorCameraPreviews[i].reset();
                    mActorCameraPreviewTextures[i].reset();
                    mCameraImages[i]->setRenderItemTexture(nullptr);
                }
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
        const std::string& session, const std::string& view, const std::string& model, float zoom,
        bool rebuildCameraTiles)
    {
        if (model.empty())
        {
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
            settings.mCameraPan = osg::Vec3f(editFloat(mPanXEdit, 0.f), 0.f, editFloat(mPanZEdit, 0.f));
            settings.mCameraTiltDegrees = editFloat(mTiltEdit, 0.f);

            auto preview = std::make_unique<MWRender::StandaloneModelPreview>(mParent, mResourceSystem, settings);
            auto previewTexture = std::make_unique<MyGUIPlatform::OSGTexture>(preview->getTexture());
            clearPendingPreviews();
            mPendingPreview = std::move(preview);
            mPendingPreviewTexture = std::move(previewTexture);

            if (rebuildCameraTiles)
            {
                std::array<std::unique_ptr<MWRender::StandaloneModelPreview>, 8> cameraPreviews;
                std::array<std::unique_ptr<MyGUIPlatform::OSGTexture>, 8> cameraPreviewTextures;
                for (std::size_t i = 0; i < sCameraTileViews.size(); ++i)
                {
                    MWRender::StandaloneModelPreviewSettings cameraSettings = settings;
                    cameraSettings.mWidth = 512;
                    cameraSettings.mHeight = 384;
                    cameraSettings.mCameraDirection = cameraDirectionForView(cameraTileView(i));
                    cameraPreviews[i]
                        = std::make_unique<MWRender::StandaloneModelPreview>(mParent, mResourceSystem, cameraSettings);
                    cameraPreviewTextures[i]
                        = std::make_unique<MyGUIPlatform::OSGTexture>(cameraPreviews[i]->getTexture());
                }
                for (std::size_t i = 0; i < sCameraTileViews.size(); ++i)
                {
                    mPendingCameraPreviews[i] = std::move(cameraPreviews[i]);
                    mPendingCameraPreviewTextures[i] = std::move(cameraPreviewTextures[i]);
                }
            }
            mPendingCameraTiles = rebuildCameraTiles;
            mPendingPreviewSeconds = 0.2f;
            mPendingPreviewFrames = 3;

            const MWRender::StandaloneModelPreviewState& state = mPendingPreview->getState();
            setStatus(std::string("loaded bounds=") + (state.mBoundsValid ? "valid" : "invalid"));
            Log(Debug::Info) << "FNV/ESM4 asset studio model loaded assetClass=\"" << assetClass << "\" record=\""
                             << record << "\" session=\"" << session << "\" view=\"" << view << "\" model=\"" << model
                             << "\" correctedModel=\"" << state.mCorrectedModel << "\" boundsValid="
                             << state.mBoundsValid << " boundsSize=(" << state.mBoundsSize.x() << ","
                             << state.mBoundsSize.y() << "," << state.mBoundsSize.z()
                             << ") camera=(" << state.mCameraPosition.x() << "," << state.mCameraPosition.y()
                             << "," << state.mCameraPosition.z()
                             << ") zoom=" << zoom << " cameraPan=(" << settings.mCameraPan.x() << ","
                             << settings.mCameraPan.z() << ") cameraTiltDeg=" << settings.mCameraTiltDegrees
                             << " threeCamera=1 cameraTileRebuild=" << rebuildCameraTiles
                             << " gate=native-asset-studio-model-preview runtime=runtime-supported";
            if (rebuildCameraTiles)
                Log(Debug::Info) << "FNV/ESM4 asset studio three camera preview assetClass=\"" << assetClass
                                 << "\" record=\"" << record << "\" session=\"" << session
                                 << "\" views=\"front,back,left,right,top,bottom,iso-nw,iso-sw\" zoom=" << zoom
                                 << " gate=native-asset-studio-three-camera-preview runtime=runtime-supported";
            return true;
        }
        catch (const std::exception& e)
        {
            if (mPreview == nullptr && mActorPreview == nullptr)
                mPreviewImage->setRenderItemTexture(nullptr);
            if (rebuildCameraTiles && mPreview == nullptr && mActorPreview == nullptr)
            {
                for (std::size_t i = 0; i < mCameraPreviews.size(); ++i)
                {
                    mCameraPreviews[i].reset();
                    mCameraPreviewTextures[i].reset();
                    mCameraImages[i]->setRenderItemTexture(nullptr);
                }
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
