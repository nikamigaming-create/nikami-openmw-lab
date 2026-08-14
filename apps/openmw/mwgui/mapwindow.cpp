#include "mapwindow.hpp"

#include <algorithm>

#include <osg/Texture2D>

#include <MyGUI_Button.h>
#include <MyGUI_FactoryManager.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_RotatingSkin.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_TextIterator.h>
#include <MyGUI_Window.h>

#include <components/esm3/esmwriter.hpp>
#include <components/esm3/globalmap.hpp>
#include <components/esm/util.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadwrld.hpp>
#include <components/debug/debuglog.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/player.hpp"
#include "../mwworld/worldmodel.hpp"

#include "../mwrender/globalmap.hpp"
#include "../mwrender/localmap.hpp"

#include "confirmationdialog.hpp"
#include "fnvmapmarker.hpp"

#include <numeric>

//## VR_PATCH BEGIN
#include <components/vr/vr.hpp>
//## VR_PATCH END

namespace
{

    constexpr float speed = 1.08f; // the zoom speed, it should be greater than 1

    bool isFalloutContentLoaded()
    {
        if (std::getenv("OPENMW_FNV_PROOF_PIPBOY_SURFACE") != nullptr)
            return true;

        const MWBase::World* world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return false;

        for (const std::string& file : world->getContentFiles())
        {
            if (file.find("FalloutNV.esm") != std::string::npos || file.find("falloutnv.esm") != std::string::npos)
                return true;
        }

        return false;
    }

    std::string getFalloutWorldMapTexture()
    {
        const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        return Misc::ResourceHelpers::correctTexturePath(
            "textures\\interface\\worldmap\\wasteland_nv_2048_no_map.dds", vfs);
    }

    std::optional<MWGui::FalloutWorldMapGeometry> getFalloutWorldMapGeometry()
    {
        const MWBase::World* world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return std::nullopt;

        const auto& worlds = world->getStore().get<ESM4::World>();
        for (std::size_t index = 0; index < worlds.getSize(); ++index)
        {
            const ESM4::World* record = worlds.at(index);
            if (record == nullptr || record->mEditorId != "WastelandNV" || record->mMap.width == 0
                || record->mMap.height == 0 || record->mMap.NWcellX >= record->mMap.SEcellX
                || record->mMap.NWcellY <= record->mMap.SEcellY)
                continue;
            return MWGui::FalloutWorldMapGeometry{ static_cast<float>(record->mMap.NWcellX),
                static_cast<float>(record->mMap.NWcellY), static_cast<float>(record->mMap.SEcellX),
                static_cast<float>(record->mMap.SEcellY), static_cast<float>(record->mMap.width),
                static_cast<float>(record->mMap.height) };
        }
        return std::nullopt;
    }

    const char* getFalloutMapModeLabel(bool global)
    {
        return global ? "MOJAVE" : "LOCAL MAP";
    }

    const char* getFalloutMapToggleLabel(bool global)
    {
        return global ? "LOCAL MAP" : "MOJAVE";
    }

    enum LocalMapWidgetDepth
    {
        Local_MarkerAboveFogLayer = 0,
        Local_CompassLayer = 1,
        Local_FogLayer = 2,
        Local_MarkerLayer = 3,
        Local_MapLayer = 4
    };

    enum GlobalMapWidgetDepth
    {
        Global_CompassLayer = 0,
        Global_MarkerLayer = 1,
        Global_ExploreOverlayLayer = 2,
        Global_MapLayer = 3
    };

    /// @brief A widget that changes its color when hovered.
    class MarkerWidget final : public MyGUI::Widget
    {
        MYGUI_RTTI_DERIVED(MarkerWidget)

    public:
        void setNormalColour(const MyGUI::Colour& colour)
        {
            mNormalColour = colour;
            setColour(colour);
        }

        void setHoverColour(const MyGUI::Colour& colour) { mHoverColour = colour; }

    private:
        MyGUI::Colour mNormalColour;
        MyGUI::Colour mHoverColour;

        void onMouseLostFocus(MyGUI::Widget* /*newWidget*/) override { setColour(mNormalColour); }

        void onMouseSetFocus(MyGUI::Widget* /*oldWidget*/) override { setColour(mHoverColour); }
    };

    MyGUI::IntRect createRect(const MyGUI::IntPoint& center, int radius)
    {
        return { center.left - radius, center.top - radius, center.left + radius, center.top + radius };
    }

    int getLocalViewingDistance()
    {
        if (!Settings::map().mAllowZooming)
            return Constants::CellGridRadius;
        if (!Settings::terrain().mDistantTerrain)
            return Constants::CellGridRadius;
        const int viewingDistanceInCells = Settings::camera().mViewingDistance / Constants::CellSizeInUnits;
        return std::clamp(
            viewingDistanceInCells, Constants::CellGridRadius, Settings::map().mMaxLocalViewingDistance.get());
    }

    ESM::RefId getCellIdInWorldSpace(const MWWorld::Cell& cell, int x, int y)
    {
        if (cell.isExterior())
            return ESM::Cell::generateIdForCell(true, {}, x, y);
        return cell.getId();
    }

    void setCanvasSize(MyGUI::ScrollView* scrollView, const MyGUI::IntRect& grid, int widgetSize)
    {
        scrollView->setCanvasSize(widgetSize * (grid.width() + 1), widgetSize * (grid.height() + 1));
    }
}

namespace MWGui
{

    void CustomMarkerCollection::addMarker(const ESM::CustomMarker& marker, bool triggerEvent)
    {
        mMarkers.insert(std::make_pair(marker.mCell, marker));
        if (triggerEvent)
            eventMarkersChanged();
    }

    void CustomMarkerCollection::deleteMarker(const ESM::CustomMarker& marker)
    {
        std::pair<ContainerType::iterator, ContainerType::iterator> range = mMarkers.equal_range(marker.mCell);

        for (ContainerType::iterator it = range.first; it != range.second; ++it)
        {
            if (it->second == marker)
            {
                mMarkers.erase(it);
                eventMarkersChanged();
                return;
            }
        }
        throw std::runtime_error("can't find marker to delete");
    }

    void CustomMarkerCollection::updateMarker(const ESM::CustomMarker& marker, const std::string& newNote)
    {
        std::pair<ContainerType::iterator, ContainerType::iterator> range = mMarkers.equal_range(marker.mCell);

        for (ContainerType::iterator it = range.first; it != range.second; ++it)
        {
            if (it->second == marker)
            {
                it->second.mNote = newNote;
                eventMarkersChanged();
                return;
            }
        }
        throw std::runtime_error("can't find marker to update");
    }

    void CustomMarkerCollection::clear()
    {
        mMarkers.clear();
        eventMarkersChanged();
    }

    CustomMarkerCollection::ContainerType::const_iterator CustomMarkerCollection::begin() const
    {
        return mMarkers.begin();
    }

    CustomMarkerCollection::ContainerType::const_iterator CustomMarkerCollection::end() const
    {
        return mMarkers.end();
    }

    CustomMarkerCollection::RangeType CustomMarkerCollection::getMarkers(const ESM::RefId& cellId) const
    {
        return mMarkers.equal_range(cellId);
    }

    size_t CustomMarkerCollection::size() const
    {
        return mMarkers.size();
    }

    // ------------------------------------------------------

    LocalMapBase::LocalMapBase(
        CustomMarkerCollection& markers, MWRender::LocalMap* localMapRender, bool fogOfWarEnabled)
        : mLocalMapRender(localMapRender)
        , mFogOfWarEnabled(fogOfWarEnabled)
        , mCustomMarkers(markers)
    {
        mCustomMarkers.eventMarkersChanged += MyGUI::newDelegate(this, &LocalMapBase::updateCustomMarkers);
    }

    LocalMapBase::~LocalMapBase()
    {
        mCustomMarkers.eventMarkersChanged -= MyGUI::newDelegate(this, &LocalMapBase::updateCustomMarkers);
    }

    MWGui::LocalMapBase::MapEntry& LocalMapBase::addMapEntry()
    {
        const int mapWidgetSize = Settings::map().mLocalMapWidgetSize;
        MyGUI::ImageBox* map = mLocalMap->createWidget<MyGUI::ImageBox>(
            "ImageBox", MyGUI::IntCoord(0, 0, mapWidgetSize, mapWidgetSize), MyGUI::Align::Top | MyGUI::Align::Left);
        map->setDepth(Local_MapLayer);

        MyGUI::ImageBox* fog = mLocalMap->createWidget<MyGUI::ImageBox>(
            "ImageBox", MyGUI::IntCoord(0, 0, mapWidgetSize, mapWidgetSize), MyGUI::Align::Top | MyGUI::Align::Left);
        fog->setDepth(Local_FogLayer);
        fog->setColour(MyGUI::Colour(0, 0, 0));

        map->setNeedMouseFocus(false);
        fog->setNeedMouseFocus(false);

        return mMaps.emplace_back(map, fog);
    }

    void LocalMapBase::init(MyGUI::ScrollView* widget, MyGUI::ImageBox* compass, int cellDistance)
    {
        mLocalMap = widget;
        mCompass = compass;
        mGrid = createRect({ 0, 0 }, cellDistance);
        mExtCellDistance = cellDistance;

        const int mapWidgetSize = Settings::map().mLocalMapWidgetSize;
        setCanvasSize(mLocalMap, mGrid, mapWidgetSize);

        mCompass->setDepth(Local_CompassLayer);
        mCompass->setNeedMouseFocus(false);

        int numCells = (mGrid.width() + 1) * (mGrid.height() + 1);
        for (int i = 0; i < numCells; ++i)
            addMapEntry();
    }

    bool LocalMapBase::toggleFogOfWar()
    {
        mFogOfWarToggled = !mFogOfWarToggled;
        applyFogOfWar();
        return mFogOfWarToggled;
    }

    void LocalMapBase::applyFogOfWar()
    {
        if (!mFogOfWarToggled || !mFogOfWarEnabled)
        {
            for (auto& entry : mMaps)
            {
                entry.mFogWidget->setImageTexture({});
                entry.mFogTexture.reset();
            }
        }

        redraw();
    }

    MyGUI::IntPoint LocalMapBase::getPosition(int cellX, int cellY, float nX, float nY) const
    {
        // normalized cell coordinates
        auto mapWidgetSize = getWidgetSize();
        return MyGUI::IntPoint(std::round((nX + cellX - mGrid.left) * mapWidgetSize),
            std::round((nY - cellY + mGrid.bottom) * mapWidgetSize));
    }

    MyGUI::IntPoint LocalMapBase::getMarkerPosition(float worldX, float worldY, MarkerUserData& markerPos) const
    {
        osg::Vec2i cellIndex;
        // normalized cell coordinates
        float nX, nY;

        if (mActiveCell->isExterior())
        {
            const ESM::RefId worldspace = mActiveCell->getWorldSpace();
            const int activeCellSize = ESM::getCellSize(worldspace);
            ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(worldX, worldY, worldspace);
            cellIndex.x() = cellPos.mX;
            cellIndex.y() = cellPos.mY;

            nX = (worldX - activeCellSize * cellIndex.x()) / activeCellSize;
            // Image space is -Y up, cells are Y up
            nY = 1 - (worldY - activeCellSize * cellIndex.y()) / activeCellSize;
        }
        else
            mLocalMapRender->worldToInteriorMapPosition({ worldX, worldY }, nX, nY, cellIndex.x(), cellIndex.y());

        markerPos.cellX = cellIndex.x();
        markerPos.cellY = cellIndex.y();
        markerPos.nX = nX;
        markerPos.nY = nY;
        return getPosition(markerPos.cellX, markerPos.cellY, markerPos.nX, markerPos.nY);
    }

    MyGUI::IntCoord LocalMapBase::getMarkerCoordinates(
        float worldX, float worldY, MarkerUserData& markerPos, size_t markerSize) const
    {
        int halfMarkerSize = markerSize / 2;
        auto position = getMarkerPosition(worldX, worldY, markerPos);
        return MyGUI::IntCoord(position.left - halfMarkerSize, position.top - halfMarkerSize, markerSize, markerSize);
    }

    MyGUI::Widget* LocalMapBase::createDoorMarker(const std::string& name, float x, float y) const
    {
        MarkerUserData data(mLocalMapRender);
        data.caption = name;
        MarkerWidget* markerWidget = mLocalMap->createWidget<MarkerWidget>(
            "MarkerButton", getMarkerCoordinates(x, y, data, 8), MyGUI::Align::Default);
        markerWidget->setNormalColour(
            MyGUI::Colour::parse(MyGUI::LanguageManager::getInstance().replaceTags("#{fontcolour=normal}")));
        markerWidget->setHoverColour(
            MyGUI::Colour::parse(MyGUI::LanguageManager::getInstance().replaceTags("#{fontcolour=normal_over}")));
        markerWidget->setDepth(Local_MarkerLayer);
        markerWidget->setNeedMouseFocus(true);
        // Used by tooltips to not show the tooltip if marker is hidden by fog of war
        markerWidget->setUserString("ToolTipType", "MapMarker");

        markerWidget->setUserData(data);
        return markerWidget;
    }

    void LocalMapBase::centerView()
    {
        MyGUI::IntPoint pos = mCompass->getPosition() + MyGUI::IntPoint{ 16, 16 };
        MyGUI::IntSize viewsize = mLocalMap->getSize();
        MyGUI::IntPoint viewOffset((viewsize.width / 2) - pos.left, (viewsize.height / 2) - pos.top);
        mLocalMap->setViewOffset(viewOffset);
    }

    MyGUI::IntCoord LocalMapBase::getMarkerCoordinates(MyGUI::Widget* widget, size_t markerSize) const
    {
        MarkerUserData& markerPos(*widget->getUserData<MarkerUserData>());
        auto position = getPosition(markerPos.cellX, markerPos.cellY, markerPos.nX, markerPos.nY);
        return MyGUI::IntCoord(position.left - markerSize / 2, position.top - markerSize / 2, markerSize, markerSize);
    }

    std::vector<MyGUI::Widget*>& LocalMapBase::currentDoorMarkersWidgets()
    {
        return mActiveCell->isExterior() ? mExteriorDoorMarkerWidgets : mInteriorDoorMarkerWidgets;
    }

    void LocalMapBase::updateCustomMarkers()
    {
        for (MyGUI::Widget* widget : mCustomMarkerWidgets)
            MyGUI::Gui::getInstance().destroyWidget(widget);
        mCustomMarkerWidgets.clear();
        if (!mActiveCell)
            return;
        auto updateMarkers = [this](CustomMarkerCollection::RangeType markers) {
            for (auto it = markers.first; it != markers.second; ++it)
            {
                const ESM::CustomMarker& marker = it->second;
                MarkerUserData markerPos(mLocalMapRender);
                MarkerWidget* markerWidget = mLocalMap->createWidget<MarkerWidget>("CustomMarkerButton",
                    getMarkerCoordinates(marker.mWorldX, marker.mWorldY, markerPos, 16), MyGUI::Align::Default);
                markerWidget->setDepth(Local_MarkerAboveFogLayer);
                markerWidget->setUserString("ToolTipType", "Layout");
                markerWidget->setUserString("ToolTipLayout", "TextToolTipOneLine");
                markerWidget->setUserString("Caption_TextOneLine", MyGUI::TextIterator::toTagsString(marker.mNote));
                markerWidget->setNormalColour(MyGUI::Colour(0.6f, 0.6f, 0.6f));
                markerWidget->setHoverColour(MyGUI::Colour(1.0f, 1.0f, 1.0f));
                markerWidget->setUserData(marker);
                markerWidget->setNeedMouseFocus(true);
                customMarkerCreated(markerWidget);
                mCustomMarkerWidgets.push_back(markerWidget);
            }
        };
        if (mActiveCell->isExterior())
        {
            for (int x = mGrid.left; x <= mGrid.right; ++x)
            {
                for (int y = mGrid.top; y <= mGrid.bottom; ++y)
                {
                    ESM::RefId cellRefId = getCellIdInWorldSpace(*mActiveCell, x, y);
                    updateMarkers(mCustomMarkers.getMarkers(cellRefId));
                }
            }
        }
        else
            updateMarkers(mCustomMarkers.getMarkers(mActiveCell->getId()));

        redraw();
    }

    void LocalMapBase::setActiveCell(const MWWorld::Cell& cell)
    {
        if (&cell == mActiveCell)
            return; // don't do anything if we're still in the same cell

        const int x = cell.getGridX();
        const int y = cell.getGridY();

        MyGUI::IntSize oldSize{ mGrid.width(), mGrid.height() };

        if (cell.isExterior())
        {
            mGrid = createRect({ x, y }, mExtCellDistance);
            const MyGUI::IntRect activeGrid = createRect({ x, y }, Constants::CellGridRadius);

            mExteriorDoorMarkerWidgets.clear();
            for (auto& [coord, doors] : mExteriorDoorsByCell)
            {
                if (!mHasALastActiveCell || !mGrid.inside({ coord.first, coord.second })
                    || activeGrid.inside({ coord.first, coord.second }))
                {
                    mDoorMarkersToRecycle.insert(mDoorMarkersToRecycle.end(), doors.begin(), doors.end());
                    doors.clear();
                }
                else
                    mExteriorDoorMarkerWidgets.insert(mExteriorDoorMarkerWidgets.end(), doors.begin(), doors.end());
            }

            for (auto& widget : mDoorMarkersToRecycle)
                widget->setVisible(false);

            if (mHasALastActiveCell)
            {
                for (const auto& entry : mMaps)
                {
                    if (!mGrid.inside({ entry.mCellX, entry.mCellY }))
                        mLocalMapRender->removeExteriorCell(entry.mCellX, entry.mCellY);
                }
            }
        }
        else
            mGrid = mLocalMapRender->getInteriorGrid();

        mActiveCell = &cell;

        constexpr auto resetEntry = [](MapEntry& entry, bool visible, const MyGUI::IntPoint* position) {
            entry.mMapWidget->setVisible(visible);
            entry.mFogWidget->setVisible(visible);
            if (position)
            {
                entry.mMapWidget->setPosition(*position);
                entry.mFogWidget->setPosition(*position);
            }
            entry.mMapWidget->setRenderItemTexture(nullptr);
            entry.mFogWidget->setRenderItemTexture(nullptr);
            entry.mMapTexture.reset();
            entry.mFogTexture.reset();
        };

        std::size_t usedEntries = 0;
        for (int cx = mGrid.left; cx <= mGrid.right; ++cx)
        {
            for (int cy = mGrid.top; cy <= mGrid.bottom; ++cy)
            {
                MapEntry& entry = usedEntries < mMaps.size() ? mMaps[usedEntries] : addMapEntry();
                entry.mCellX = cx;
                entry.mCellY = cy;
                MyGUI::IntPoint position = getPosition(cx, cy, 0, 0);
                resetEntry(entry, true, &position);
                ++usedEntries;
            }
        }
        for (std::size_t i = usedEntries; i < mMaps.size(); ++i)
        {
            resetEntry(mMaps[i], false, nullptr);
        }

        if (oldSize != MyGUI::IntSize{ mGrid.width(), mGrid.height() })
            setCanvasSize(mLocalMap, mGrid, getWidgetSize());

        // Delay the door markers update until scripts have been given a chance to run.
        // If we don't do this, door markers that should be disabled will still appear on the map.
        mNeedDoorMarkersUpdate = true;

        for (MyGUI::Widget* widget : currentDoorMarkersWidgets())
            widget->setCoord(getMarkerCoordinates(widget, 8));

        if (mActiveCell->isExterior())
            mHasALastActiveCell = true;

        updateMagicMarkers();
        updateCustomMarkers();
    }

    void LocalMapBase::requestMapRender(const MWWorld::CellStore* cell)
    {
        mLocalMapRender->requestMap(cell);
    }

    void LocalMapBase::redraw()
    {
        // Redraw children in proper order
        mLocalMap->getParent()->_updateChilds();
    }

    float LocalMapBase::getWidgetSize() const
    {
        return mLocalMapZoom * Settings::map().mLocalMapWidgetSize;
    }

    void LocalMapBase::setPlayerPos(int cellX, int cellY, const float nx, const float ny)
    {
        MyGUI::IntPoint pos = getPosition(cellX, cellY, nx, ny) - MyGUI::IntPoint{ 16, 16 };
        static bool loggedFalloutLocalTracking = false;
        if (!loggedFalloutLocalTracking && isFalloutContentLoaded())
        {
            loggedFalloutLocalTracking = true;
            Log(Debug::Info) << "FNV/ESM4 proof: local map tracks player cell=(" << cellX << "," << cellY
                             << ") norm=(" << nx << "," << ny << ") arrow=(" << pos.left << "," << pos.top
                             << ")";
        }

        if (pos != mCompass->getPosition())
        {
            notifyPlayerUpdate();

            mCompass->setPosition(pos);
        }
        const int activeCellSize = mActiveCell ? ESM::getCellSize(mActiveCell->getWorldSpace()) : Constants::CellSizeInUnits;
        osg::Vec2f curPos((cellX + nx) * activeCellSize, (cellY + 1 - ny) * activeCellSize);
        if ((curPos - mCurPos).length2() > 0.001)
        {
            mCurPos = curPos;
            centerView();
        }
    }

    void LocalMapBase::setPlayerDir(const float x, const float y)
    {
        if (x == mLastDirectionX && y == mLastDirectionY)
            return;

        notifyPlayerUpdate();

        MyGUI::ISubWidget* main = mCompass->getSubWidgetMain();
        MyGUI::RotatingSkin* rotatingSubskin = main->castType<MyGUI::RotatingSkin>();
        rotatingSubskin->setCenter(MyGUI::IntPoint(16, 16));
        float angle = std::atan2(x, y);
        rotatingSubskin->setAngle(angle);

        mLastDirectionX = x;
        mLastDirectionY = y;
    }

    void LocalMapBase::addDetectionMarkers(int type)
    {
        std::vector<MWWorld::Ptr> markers;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        world->listDetectedReferences(world->getPlayerPtr(), markers, MWBase::World::DetectionType(type));
        if (markers.empty())
            return;

        std::string_view markerTexture;
        if (type == MWBase::World::Detect_Creature)
        {
            markerTexture = "textures\\detect_animal_icon.dds";
        }
        if (type == MWBase::World::Detect_Key)
        {
            markerTexture = "textures\\detect_key_icon.dds";
        }
        if (type == MWBase::World::Detect_Enchantment)
        {
            markerTexture = "textures\\detect_enchantment_icon.dds";
        }

        for (const MWWorld::Ptr& ptr : markers)
        {
            const ESM::Position& worldPos = ptr.getRefData().getPosition();
            MarkerUserData markerPos(mLocalMapRender);
            MyGUI::ImageBox* markerWidget = mLocalMap->createWidget<MyGUI::ImageBox>("ImageBox",
                getMarkerCoordinates(worldPos.pos[0], worldPos.pos[1], markerPos, 8), MyGUI::Align::Default);
            markerWidget->setDepth(Local_MarkerAboveFogLayer);
            markerWidget->setImageTexture(markerTexture);
            markerWidget->setImageCoord(MyGUI::IntCoord(0, 0, 8, 8));
            markerWidget->setNeedMouseFocus(false);
            markerWidget->setUserData(markerPos);
            mMagicMarkerWidgets.push_back(markerWidget);
        }
    }

    void LocalMapBase::onFrame(float dt)
    {
        if (mNeedDoorMarkersUpdate)
        {
            updateDoorMarkers();
            mNeedDoorMarkersUpdate = false;
        }

        mMarkerUpdateTimer += dt;

        if (mMarkerUpdateTimer >= 0.25)
        {
            mMarkerUpdateTimer = 0;
            updateMagicMarkers();
        }

        updateRequiredMaps();
    }

    bool widgetCropped(MyGUI::Widget* widget, MyGUI::Widget* cropTo)
    {
        MyGUI::IntRect coord = widget->getAbsoluteRect();
        MyGUI::IntRect croppedCoord = cropTo->getAbsoluteRect();
        return !coord.intersect(croppedCoord);
    }

    void LocalMapBase::updateRequiredMaps()
    {
        bool needRedraw = false;
        for (MapEntry& entry : mMaps)
        {
            if (widgetCropped(entry.mMapWidget, mLocalMap))
                continue;

            if (!entry.mMapTexture)
            {
                if (mActiveCell->isExterior())
                {
                    const ESM::RefId worldspace = mActiveCell->getWorldSpace();
                    requestMapRender(&MWBase::Environment::get().getWorldModel()->getExterior(
                        ESM::ExteriorCellLocation(entry.mCellX, entry.mCellY, worldspace)));
                }

//## VR_PATCH BEGIN
// Support Texture2DArray
                osg::ref_ptr<osg::Texture> texture = mLocalMapRender->getMapTexture(entry.mCellX, entry.mCellY);
//## VR_PATCH END
                if (texture)
                {
                    entry.mMapTexture = std::make_unique<MyGUIPlatform::OSGTexture>(texture);
                    entry.mMapWidget->setRenderItemTexture(entry.mMapTexture.get());
                    entry.mMapWidget->setColour(MyGUI::Colour::White);
                    entry.mMapWidget->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));
                    needRedraw = true;
                    if (isFalloutContentLoaded())
                    {
                        static int loggedHits = 0;
                        if (loggedHits < 12)
                        {
                            Log(Debug::Info) << "FNV/ESM4 proof: local map tile texture ready cell=("
                                             << entry.mCellX << "," << entry.mCellY << ") widget="
                                             << entry.mMapWidget->getCoord().left << ","
                                             << entry.mMapWidget->getCoord().top << ","
                                             << entry.mMapWidget->getCoord().width << ","
                                             << entry.mMapWidget->getCoord().height;
                            ++loggedHits;
                        }
                    }
                }
                else
                {
                    if (isFalloutContentLoaded())
                    {
                        entry.mMapWidget->setImageTexture("black");
                        entry.mMapWidget->setColour(MyGUI::Colour(0.04f, 0.22f, 0.08f, 0.85f));
                        needRedraw = true;
                        static int loggedMisses = 0;
                        if (loggedMisses < 12)
                        {
                            Log(Debug::Verbose) << "FNV/ESM4 diag: local map tile texture pending cell=("
                                             << entry.mCellX << "," << entry.mCellY << ") visible="
                                             << entry.mMapWidget->getVisible() << " localVisible="
                                             << mLocalMap->getVisible();
                            ++loggedMisses;
                        }
                    }
                    if (!isFalloutContentLoaded())
                        entry.mMapTexture = std::make_unique<MyGUIPlatform::OSGTexture>(std::string(), nullptr);
                }
            }
            if (!entry.mFogTexture && mFogOfWarToggled && mFogOfWarEnabled)
            {
//## VR_PATCH BEGIN
// Support Texture2DArray
                osg::ref_ptr<osg::Texture> tex = mLocalMapRender->getFogOfWarTexture(entry.mCellX, entry.mCellY);
//## VR_PATCH END
                if (tex)
                {
                    entry.mFogTexture = std::make_unique<MyGUIPlatform::OSGTexture>(tex);
                    entry.mFogWidget->setRenderItemTexture(entry.mFogTexture.get());
                    entry.mFogWidget->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));
                }
                else
                {
                    if (isFalloutContentLoaded())
                        entry.mFogWidget->setVisible(false);
                    else
                        entry.mFogWidget->setImageTexture("black");
                    entry.mFogTexture = std::make_unique<MyGUIPlatform::OSGTexture>(std::string(), nullptr);
                }
                needRedraw = true;
            }
        }
        if (needRedraw)
            redraw();
    }

    void LocalMapBase::updateDoorMarkers()
    {
        std::vector<MWBase::World::DoorMarker> doors;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWWorld::WorldModel* worldModel = MWBase::Environment::get().getWorldModel();

        mDoorMarkersToRecycle.insert(
            mDoorMarkersToRecycle.end(), mInteriorDoorMarkerWidgets.begin(), mInteriorDoorMarkerWidgets.end());
        mInteriorDoorMarkerWidgets.clear();

        if (!mActiveCell->isExterior())
        {
            for (MyGUI::Widget* widget : mExteriorDoorMarkerWidgets)
                widget->setVisible(false);

            MWWorld::CellStore& cell = worldModel->getInterior(mActiveCell->getNameId());
            world->getDoorMarkers(cell, doors);
        }
        else
        {
            for (MapEntry& entry : mMaps)
            {
                if (!entry.mMapTexture && !widgetCropped(entry.mMapWidget, mLocalMap))
                    world->getDoorMarkers(worldModel->getExterior(ESM::ExteriorCellLocation(
                                              entry.mCellX, entry.mCellY, ESM::Cell::sDefaultWorldspaceId)),
                        doors);
            }
            if (doors.empty())
                return;
        }

        // Create a widget for each marker
        for (MWBase::World::DoorMarker& marker : doors)
        {
            std::vector<std::string> destNotes;
            CustomMarkerCollection::RangeType markers = mCustomMarkers.getMarkers(marker.dest);
            for (CustomMarkerCollection::ContainerType::const_iterator iter = markers.first; iter != markers.second;
                 ++iter)
                destNotes.push_back(iter->second.mNote);

            MyGUI::Widget* markerWidget = nullptr;
            MarkerUserData* data;
            if (mDoorMarkersToRecycle.empty())
            {
                markerWidget = createDoorMarker(marker.name, marker.x, marker.y);
                data = markerWidget->getUserData<MarkerUserData>();
                data->notes = std::move(destNotes);
                doorMarkerCreated(markerWidget);
            }
            else
            {
                markerWidget = (MarkerWidget*)mDoorMarkersToRecycle.back();
                mDoorMarkersToRecycle.pop_back();

                data = markerWidget->getUserData<MarkerUserData>();
                data->notes = std::move(destNotes);
                data->caption = marker.name;
                markerWidget->setCoord(getMarkerCoordinates(marker.x, marker.y, *data, 8));
                markerWidget->setVisible(true);
            }

            currentDoorMarkersWidgets().push_back(markerWidget);
            if (mActiveCell->isExterior())
                mExteriorDoorsByCell[{ data->cellX, data->cellY }].push_back(markerWidget);
        }

        for (auto& widget : mDoorMarkersToRecycle)
            widget->setVisible(false);
    }

    void LocalMapBase::updateMagicMarkers()
    {
        // clear all previous markers
        for (MyGUI::Widget* widget : mMagicMarkerWidgets)
            MyGUI::Gui::getInstance().destroyWidget(widget);
        mMagicMarkerWidgets.clear();

        addDetectionMarkers(MWBase::World::Detect_Creature);
        addDetectionMarkers(MWBase::World::Detect_Key);
        addDetectionMarkers(MWBase::World::Detect_Enchantment);

        // Add marker for the spot marked with Mark magic effect
        MWWorld::CellStore* markedCell = nullptr;
        ESM::Position markedPosition;
        MWBase::Environment::get().getWorld()->getPlayer().getMarkedPosition(markedCell, markedPosition);
        if (markedCell && markedCell->getCell()->getWorldSpace() == mActiveCell->getWorldSpace())
        {
            MarkerUserData markerPos(mLocalMapRender);
            MyGUI::ImageBox* markerWidget = mLocalMap->createWidget<MyGUI::ImageBox>("ImageBox",
                getMarkerCoordinates(markedPosition.pos[0], markedPosition.pos[1], markerPos, 8),
                MyGUI::Align::Default);
            markerWidget->setDepth(Local_MarkerAboveFogLayer);
            markerWidget->setImageTexture("textures\\menu_map_smark.dds");
            markerWidget->setNeedMouseFocus(false);
            markerWidget->setUserData(markerPos);
            mMagicMarkerWidgets.push_back(markerWidget);
        }

        redraw();
    }

    void LocalMapBase::updateLocalMap()
    {
        auto mapWidgetSize = getWidgetSize();
        setCanvasSize(mLocalMap, mGrid, getWidgetSize());

        const auto size = MyGUI::IntSize(std::ceil(mapWidgetSize), std::ceil(mapWidgetSize));
        for (auto& entry : mMaps)
        {
            const auto position = getPosition(entry.mCellX, entry.mCellY, 0, 0);
            entry.mMapWidget->setCoord({ position, size });
            entry.mFogWidget->setCoord({ position, size });
        }

        MarkerUserData markerPos(mLocalMapRender);
        for (MyGUI::Widget* widget : currentDoorMarkersWidgets())
            widget->setCoord(getMarkerCoordinates(widget, 8));

        for (MyGUI::Widget* widget : mCustomMarkerWidgets)
        {
            const auto& marker = *widget->getUserData<ESM::CustomMarker>();
            widget->setCoord(getMarkerCoordinates(marker.mWorldX, marker.mWorldY, markerPos, 16));
        }

        for (MyGUI::Widget* widget : mMagicMarkerWidgets)
            widget->setCoord(getMarkerCoordinates(widget, 8));
    }

    // ------------------------------------------------------------------------------------------
    MapWindow::MapWindow(CustomMarkerCollection& customMarkers, DragAndDrop* drag, MWRender::LocalMap* localMapRender,
        SceneUtil::WorkQueue* workQueue)
//## VR_PATCH BEGIN
        : WindowPinnableBase(VR::getVR() ? "openmw_map_window_vr.layout" : "openmw_map_window.layout")
//## VR_PATCH END
        , LocalMapBase(customMarkers, localMapRender, true)
        , NoDrop(drag, mMainWidget)
        , mGlobalMap(nullptr)
        , mGlobalMapImage(nullptr)
        , mGlobalMapOverlay(nullptr)
        , mEventBoxGlobal(nullptr)
        , mEventBoxLocal(nullptr)
        , mGlobalMapRender(std::make_unique<MWRender::GlobalMap>(localMapRender->getRoot(), workQueue))
        , mEditNoteDialog()
    {
        [[maybe_unused]] static const bool registered = [] {
            MyGUI::FactoryManager::getInstance().registerFactory<MarkerWidget>("Widget");
            return true;
        }();

        mEditNoteDialog.setVisible(false);
        mEditNoteDialog.eventOkClicked += MyGUI::newDelegate(this, &MapWindow::onNoteEditOk);
        mEditNoteDialog.eventDeleteClicked += MyGUI::newDelegate(this, &MapWindow::onNoteEditDelete);

        setCoord(500, 0, 320, 300);

        getWidget(mLocalMap, "LocalMap");
        getWidget(mGlobalMap, "GlobalMap");
        getWidget(mGlobalMapImage, "GlobalMapImage");
        getWidget(mGlobalMapOverlay, "GlobalMapOverlay");
        getWidget(mPlayerArrowLocal, "CompassLocal");
        getWidget(mPlayerArrowGlobal, "CompassGlobal");

        mPlayerArrowGlobal->setDepth(Global_CompassLayer);
        mPlayerArrowGlobal->setNeedMouseFocus(false);
        mGlobalMapImage->setDepth(Global_MapLayer);
        mGlobalMapOverlay->setDepth(Global_ExploreOverlayLayer);

        mLastScrollWindowCoordinates = mLocalMap->getCoord();
        mLocalMap->eventChangeCoord += MyGUI::newDelegate(this, &MapWindow::onChangeScrollWindowCoord);

        mGlobalMap->setVisible(false);

        getWidget(mButton, "WorldButton");
        mButton->eventMouseButtonClick += MyGUI::newDelegate(this, &MapWindow::onWorldButtonClicked);

        bool falloutContent = isFalloutContentLoaded();
        if (falloutContent)
            Settings::map().mGlobal.set(true);
        const bool global = Settings::map().mGlobal;

        if (falloutContent)
        {
            mButton->setCaption(getFalloutMapToggleLabel(global));
            Log(Debug::Info) << "FNV/ESM4 proof: map toggle label applied " << getFalloutMapModeLabel(global)
                             << " button=" << getFalloutMapToggleLabel(global);
            const std::string mapTexture = getFalloutWorldMapTexture();
            mGlobalMapTexture = std::make_unique<MyGUIPlatform::OSGTexture>(
                mapTexture, MWBase::Environment::get().getResourceSystem()->getImageManager());
            mGlobalMapTexture->loadFromFile(mapTexture);
            mGlobalMapImage->setRenderItemTexture(mGlobalMapTexture.get());
            mGlobalMapImage->setImageTexture(mapTexture);
            mGlobalMapImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 0.f, 1.f, 1.f));
            const std::optional<FalloutWorldMapGeometry> geometry = getFalloutWorldMapGeometry();
            if (!geometry)
                throw std::runtime_error("Fallout WastelandNV has no authored MNAM world-map geometry");
            const int mapWidth = static_cast<int>(geometry->mWidth);
            const int mapHeight = static_cast<int>(geometry->mHeight);
            mGlobalMapImage->setImageCoord(MyGUI::IntCoord(0, 0, mapWidth, mapHeight));
            mGlobalMapOverlay->setVisible(false);
            mGlobalMapOverlay->setImageTexture({});
            mGlobalMapOverlay->setImageCoord(MyGUI::IntCoord(0, 0, mapWidth, mapHeight));
            mGlobalMapImage->setSize(mapWidth, mapHeight);
            mGlobalMapOverlay->setSize(mapWidth, mapHeight);
            mGlobalMap->setCanvasSize(mapWidth, mapHeight);
            mGlobalMap->setViewOffset(MyGUI::IntPoint(0, 0));
            Log(Debug::Info) << "FNV/ESM4 proof: Fallout world map texture bound " << mapTexture << " authoredMNAM="
                             << mapWidth << "x" << mapHeight << " cellsNW=(" << geometry->mNorthWestCellX << ","
                             << geometry->mNorthWestCellY << ") cellsSE=(" << geometry->mSouthEastCellX << ","
                             << geometry->mSouthEastCellY << ")";
        }
        else
            mButton->setCaptionWithReplacing(global ? "#{sLocal}" : "#{sWorld}");

        getWidget(mEventBoxGlobal, "EventBoxGlobal");
        mEventBoxGlobal->eventMouseDrag += MyGUI::newDelegate(this, &MapWindow::onMouseDrag);
        mEventBoxGlobal->eventMouseButtonPressed += MyGUI::newDelegate(this, &MapWindow::onDragStart);

        const bool allowZooming = Settings::map().mAllowZooming;

        if (allowZooming)
            mEventBoxGlobal->eventMouseWheel += MyGUI::newDelegate(this, &MapWindow::onMapZoomed);
        mEventBoxGlobal->setDepth(Global_ExploreOverlayLayer);

        getWidget(mEventBoxLocal, "EventBoxLocal");
        mEventBoxLocal->eventMouseDrag += MyGUI::newDelegate(this, &MapWindow::onMouseDrag);
        mEventBoxLocal->eventMouseButtonPressed += MyGUI::newDelegate(this, &MapWindow::onDragStart);
        mEventBoxLocal->eventMouseButtonDoubleClick += MyGUI::newDelegate(this, &MapWindow::onMapDoubleClicked);
        if (allowZooming)
            mEventBoxLocal->eventMouseWheel += MyGUI::newDelegate(this, &MapWindow::onMapZoomed);

        LocalMapBase::init(mLocalMap, mPlayerArrowLocal, getLocalViewingDistance());

        mGlobalMap->setVisible(global);
        mLocalMap->setVisible(!global);

        if (Settings::gui().mControllerMenus)
        {
            mControllerButtons.mB = "#{Interface:Back}";
            mControllerButtons.mX = global ? "#{Interface:Local}" : "#{Interface:World}";
            mControllerButtons.mY = "#{Interface:Center}";
            if (!Settings::map().mAllowZooming)
                mControllerButtons.mDpad = "#{Interface:Move}";
        }
    }

    void MapWindow::onNoteEditOk()
    {
        if (mEditNoteDialog.getDeleteButtonShown())
            mCustomMarkers.updateMarker(mEditingMarker, mEditNoteDialog.getText());
        else
        {
            mEditingMarker.mNote = mEditNoteDialog.getText();
            mCustomMarkers.addMarker(mEditingMarker);
        }

        mEditNoteDialog.setVisible(false);
    }

    void MapWindow::onNoteEditDelete()
    {
        ConfirmationDialog* confirmation = MWBase::Environment::get().getWindowManager()->getConfirmationDialog();
        confirmation->askForConfirmation("#{sDeleteNote}");
        confirmation->eventCancelClicked.clear();
        confirmation->eventOkClicked.clear();
        confirmation->eventOkClicked += MyGUI::newDelegate(this, &MapWindow::onNoteEditDeleteConfirm);
    }

    void MapWindow::onNoteEditDeleteConfirm()
    {
        mCustomMarkers.deleteMarker(mEditingMarker);

        mEditNoteDialog.setVisible(false);
    }

    void MapWindow::onCustomMarkerDoubleClicked(MyGUI::Widget* sender)
    {
        mEditingMarker = *sender->getUserData<ESM::CustomMarker>();
        mEditNoteDialog.setText(mEditingMarker.mNote);
        mEditNoteDialog.showDeleteButton(true);
        mEditNoteDialog.setVisible(true);
    }

    void MapWindow::onMapDoubleClicked(MyGUI::Widget* /*sender*/)
    {
        MyGUI::IntPoint clickedPos = MyGUI::InputManager::getInstance().getMousePosition();

        MyGUI::IntPoint widgetPos = clickedPos - mEventBoxLocal->getAbsolutePosition();
        auto mapWidgetSize = getWidgetSize();
        int x = int(widgetPos.left / float(mapWidgetSize)) + mGrid.left;
        int y = mGrid.bottom - int(widgetPos.top / float(mapWidgetSize));
        float nX = widgetPos.left / float(mapWidgetSize) - int(widgetPos.left / float(mapWidgetSize));
        float nY = widgetPos.top / float(mapWidgetSize) - int(widgetPos.top / float(mapWidgetSize));

        osg::Vec2f worldPos;
        if (!mActiveCell->isExterior())
        {
            worldPos = mLocalMapRender->interiorMapToWorldPosition(nX, nY, x, y);
        }
        else
        {
            const int activeCellSize = ESM::getCellSize(mActiveCell->getWorldSpace());
            worldPos.x() = (x + nX) * activeCellSize;
            worldPos.y() = (y + (1.0f - nY)) * activeCellSize;
        }

        mEditingMarker.mWorldX = worldPos.x();
        mEditingMarker.mWorldY = worldPos.y();
        ESM::RefId clickedId = getCellIdInWorldSpace(*mActiveCell, x, y);

        mEditingMarker.mCell = clickedId;

        mEditNoteDialog.setVisible(true);
        mEditNoteDialog.showDeleteButton(false);
        mEditNoteDialog.setText({});
    }

    void MapWindow::onMapZoomed(MyGUI::Widget* /*sender*/, int rel)
    {
        const int localWidgetSize = Settings::map().mLocalMapWidgetSize;
        const bool zoomOut = rel < 0;
        const bool zoomIn = !zoomOut;
        const double speedDiff = zoomOut ? 1.0 / speed : speed;

        const float currentMinLocalMapZoom
            = std::max({ (float(Settings::map().mGlobalMapCellSize) * 4.f) / float(localWidgetSize),
                float(mLocalMap->getWidth()) / (localWidgetSize * (mGrid.width() + 1)),
                float(mLocalMap->getHeight()) / (localWidgetSize * (mGrid.height() + 1)) });

        if (Settings::map().mGlobal)
        {
            const float currentGlobalZoom = mGlobalMapZoom;
            float sourceWidth = static_cast<float>(mGlobalMapRender->getWidth());
            float sourceHeight = static_cast<float>(mGlobalMapRender->getHeight());
            if (const std::optional<FalloutWorldMapGeometry> geometry = getFalloutWorldMapGeometry())
            {
                sourceWidth = geometry->mWidth;
                sourceHeight = geometry->mHeight;
            }
            const float currentMinGlobalMapZoom = std::min(
                float(mGlobalMap->getWidth()) / sourceWidth, float(mGlobalMap->getHeight()) / sourceHeight);

            mGlobalMapZoom *= speedDiff;

            if (zoomIn && mGlobalMapZoom > 4.f)
            {
                mGlobalMapZoom = currentGlobalZoom;
                mLocalMapZoom = currentMinLocalMapZoom;
                onWorldButtonClicked(nullptr);
                updateLocalMap();
                return; // the zoom in is too big
            }

            if (zoomOut && mGlobalMapZoom < currentMinGlobalMapZoom)
            {
                mGlobalMapZoom = currentGlobalZoom;
                return; // the zoom out is too big, we have reach the borders of the widget
            }
        }
        else
        {
            auto const currentLocalZoom = mLocalMapZoom;
            mLocalMapZoom *= speedDiff;

            if (zoomIn && mLocalMapZoom > 4.0f)
            {
                mLocalMapZoom = currentLocalZoom;
                return; // the zoom in is too big
            }

            if (zoomOut && mLocalMapZoom < currentMinLocalMapZoom)
            {
                mLocalMapZoom = currentLocalZoom;

                float zoomRatio = 4.f / mGlobalMapZoom;
                mGlobalMapZoom = 4.f;
                onWorldButtonClicked(nullptr);

                zoomOnCursor(zoomRatio);
                return; // the zoom out is too big, we switch to the global map
            }

            if (zoomOut)
                mNeedDoorMarkersUpdate = true;
        }
        zoomOnCursor(speedDiff);
    }

    void MapWindow::zoomOnCursor(float speedDiff)
    {
        auto map = Settings::map().mGlobal ? mGlobalMap : mLocalMap;
        auto cursor = MyGUI::InputManager::getInstance().getMousePosition() - map->getAbsolutePosition();
        auto centerView = map->getViewOffset() - cursor;

        Settings::map().mGlobal ? updateGlobalMap() : updateLocalMap();

        map->setViewOffset(MyGUI::IntPoint(std::round(centerView.left * speedDiff) + cursor.left,
            std::round(centerView.top * speedDiff) + cursor.top));
    }

    void MapWindow::updateGlobalMap()
    {
        if (isFalloutContentLoaded())
        {
            // FNV uses the authored Mojave texture directly. Never composite OpenMW's generated exploration/local
            // map layer over it.
            mLocalMap->setVisible(false);
            mGlobalMapOverlay->setVisible(false);
            mGlobalMapOverlay->setImageTexture({});
            const std::optional<FalloutWorldMapGeometry> geometry = getFalloutWorldMapGeometry();
            if (!geometry)
                return;
            const MyGUI::IntSize size(
                static_cast<int>(geometry->mWidth * mGlobalMapZoom),
                static_cast<int>(geometry->mHeight * mGlobalMapZoom));
            mGlobalMap->setCanvasSize(size);
            mGlobalMapImage->setSize(size);
            mGlobalMapOverlay->setSize(size);
            for (const auto& [id, widget] : mFalloutMapMarkers)
            {
                const ESM4::Reference* marker
                    = MWBase::Environment::get().getWorld()->getStore().get<ESM4::Reference>().search(id);
                if (marker == nullptr)
                    continue;
                float imageX = 0.f;
                float imageY = 0.f;
                worldPosToGlobalMapImageSpace(marker->mPos.pos[0], marker->mPos.pos[1], imageX, imageY);
                widget->setPosition(
                    MyGUI::IntPoint(static_cast<int>(imageX) - widget->getWidth() / 2,
                        static_cast<int>(imageY) - widget->getHeight() / 2));
            }
            globalMapUpdatePlayer();
            return;
        }

        resizeGlobalMap();

        float x = mCurPos.x(), y = mCurPos.y();
        if (!mActiveCell->isExterior())
        {
            auto pos = MWBase::Environment::get().getWorld()->getPlayer().getLastKnownExteriorPosition();
            x = pos.x();
            y = pos.y();
        }
        setGlobalMapPlayerPosition(x, y);

        for (auto& [marker, col] : mGlobalMapMarkers)
        {
            marker.widget->setCoord(createMarkerCoords(marker.position.x(), marker.position.y(), col.size()));
            marker.widget->setVisible(marker.widget->getHeight() >= 6);
        }
    }

    void MapWindow::onChangeScrollWindowCoord(MyGUI::Widget* sender)
    {
        MyGUI::IntCoord currentCoordinates = sender->getCoord();

        MyGUI::IntPoint currentViewPortCenter
            = MyGUI::IntPoint(currentCoordinates.width / 2, currentCoordinates.height / 2);
        MyGUI::IntPoint lastViewPortCenter
            = MyGUI::IntPoint(mLastScrollWindowCoordinates.width / 2, mLastScrollWindowCoordinates.height / 2);
        MyGUI::IntPoint viewPortCenterDiff = currentViewPortCenter - lastViewPortCenter;

        mLocalMap->setViewOffset(mLocalMap->getViewOffset() + viewPortCenterDiff);
        mGlobalMap->setViewOffset(mGlobalMap->getViewOffset() + viewPortCenterDiff);

        mLastScrollWindowCoordinates = currentCoordinates;
    }

    void MapWindow::setVisible(bool visible)
    {
        WindowBase::setVisible(visible);
        MWGui::GuiMode mode = MWBase::Environment::get().getWindowManager()->getMode();
        mButton->setVisible(visible && mode != MWGui::GM_None);

        if (Settings::gui().mControllerMenus && mode == MWGui::GM_None && pinned() && visible)
        {
            // Restore the window to pinned size.
            MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            const float x = Settings::windows().mMapX * viewSize.width;
            const float y = Settings::windows().mMapY * viewSize.height;
            const float w = Settings::windows().mMapW * viewSize.width;
            const float h = Settings::windows().mMapH * viewSize.height;
            window->setCoord(x, y, w, h);
        }
    }

    void MapWindow::renderGlobalMap()
    {
        mGlobalMapRender->render();
        resizeGlobalMap();
    }

    MapWindow::~MapWindow() = default;

    void MapWindow::setCellName(const std::string& cellName)
    {
        setTitle("#{sCell=" + cellName + "}");
    }

    MyGUI::IntCoord MapWindow::createMarkerCoords(float x, float y, float agregatedWeight) const
    {
        float worldX, worldY;
        worldPosToGlobalMapImageSpace(
            (x + 0.5f) * Constants::CellSizeInUnits, (y + 0.5f) * Constants::CellSizeInUnits, worldX, worldY);

        const float markerSize = getMarkerSize(agregatedWeight);
        const float halfMarkerSize = markerSize / 2.0f;
        return MyGUI::IntCoord(static_cast<int>(worldX - halfMarkerSize), static_cast<int>(worldY - halfMarkerSize),
            markerSize, markerSize);
    }

    MyGUI::Widget* MapWindow::createMarker(const std::string& name, float x, float y, float agregatedWeight)
    {
        MyGUI::Widget* markerWidget = mGlobalMap->createWidget<MyGUI::Widget>(
            "MarkerButton", createMarkerCoords(x, y, agregatedWeight), MyGUI::Align::Default);
        markerWidget->setVisible(markerWidget->getHeight() >= 6.0);
        markerWidget->setUserString("Caption_TextOneLine", "#{sCell=" + name + "}");
        setGlobalMapMarkerTooltip(markerWidget, x, y);

        markerWidget->setUserString("ToolTipLayout", "TextToolTipOneLine");

        markerWidget->setNeedMouseFocus(true);
        markerWidget->setColour(
            MyGUI::Colour::parse(MyGUI::LanguageManager::getInstance().replaceTags("#{fontcolour=normal}")));
        markerWidget->setDepth(Global_MarkerLayer);
        markerWidget->eventMouseDrag += MyGUI::newDelegate(this, &MapWindow::onMouseDrag);
        if (Settings::map().mAllowZooming)
            markerWidget->eventMouseWheel += MyGUI::newDelegate(this, &MapWindow::onMapZoomed);
        markerWidget->eventMouseButtonPressed += MyGUI::newDelegate(this, &MapWindow::onDragStart);

        return markerWidget;
    }

    MyGUI::Widget* MapWindow::createFalloutMapMarker(const ESM4::Reference& marker)
    {
        float imageX = 0.f;
        float imageY = 0.f;
        worldPosToGlobalMapImageSpace(marker.mPos.pos[0], marker.mPos.pos[1], imageX, imageY);
        constexpr int markerSize = 32;
        MyGUI::ImageBox* markerWidget = mGlobalMap->createWidget<MyGUI::ImageBox>("ImageBox",
            MyGUI::IntCoord(static_cast<int>(imageX) - markerSize / 2,
                static_cast<int>(imageY) - markerSize / 2, markerSize, markerSize),
            MyGUI::Align::Default);
        const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        const std::string texture
            = Misc::ResourceHelpers::correctTexturePath(getFalloutMapMarkerIcon(marker.mMapMarkerType), vfs);
        markerWidget->setImageTexture(texture);
        markerWidget->setUserString("Caption_TextOneLine", marker.mFullName);
        markerWidget->setUserString("ToolTipType", "Layout");
        markerWidget->setUserString("ToolTipLayout", "TextToolTipOneLine");
        markerWidget->setNeedMouseFocus(true);
        markerWidget->setUserData(marker.mId);
        // DDS map icons carry their own retail colour. Tinting them with the Morrowind font colour can make
        // them effectively black against the Mojave map.
        markerWidget->setColour(MyGUI::Colour::White);
        markerWidget->setAlpha(1.f);
        markerWidget->setVisible(true);
        markerWidget->setDepth(Global_MarkerLayer);
        markerWidget->eventMouseDrag += MyGUI::newDelegate(this, &MapWindow::onMouseDrag);
        markerWidget->eventMouseButtonPressed += MyGUI::newDelegate(this, &MapWindow::onDragStart);
        markerWidget->eventMouseButtonClick += MyGUI::newDelegate(this, &MapWindow::onFalloutMapMarkerClicked);
        if (Settings::map().mAllowZooming)
            markerWidget->eventMouseWheel += MyGUI::newDelegate(this, &MapWindow::onMapZoomed);
        return markerWidget;
    }

    void MapWindow::onFalloutMapMarkerClicked(MyGUI::Widget* sender)
    {
        requestFalloutFastTravel(*sender->getUserData<ESM::FormId>());
    }

    bool MapWindow::requestFalloutFastTravel(ESM::FormId markerId)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const ESM4::Reference* marker = world->getStore().get<ESM4::Reference>().search(markerId);
        if (marker == nullptr || world->getFalloutMapMarkerState(markerId) != 2)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("You have not discovered that location.");
            return false;
        }

        mPendingFalloutFastTravelMarker = markerId;
        ConfirmationDialog* confirmation = MWBase::Environment::get().getWindowManager()->getConfirmationDialog();
        confirmation->askForConfirmation("Fast travel to " + marker->mFullName + "?");
        confirmation->eventCancelClicked.clear();
        confirmation->eventOkClicked.clear();
        confirmation->eventOkClicked += MyGUI::newDelegate(this, &MapWindow::onFalloutFastTravelConfirmed);
        return true;
    }

    void MapWindow::confirmFalloutFastTravel()
    {
        onFalloutFastTravelConfirmed();
    }

    void MapWindow::onFalloutFastTravelConfirmed()
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        std::string error;
        if (!world->fastTravelToFalloutMapMarker(mPendingFalloutFastTravelMarker, error))
        {
            MWBase::Environment::get().getWindowManager()->messageBox(error);
            return;
        }

        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Inventory);
        mPendingFalloutFastTravelMarker = {};
    }

    void MapWindow::refreshFalloutMapMarkers()
    {
        for (const auto& [id, widget] : mFalloutMapMarkers)
            MyGUI::Gui::getInstance().destroyWidget(widget);
        mFalloutMapMarkers.clear();

        MWBase::World* world = MWBase::Environment::tryGetWorld();
        if (world == nullptr || !isFalloutContentLoaded())
            return;
        const auto& references = world->getStore().get<ESM4::Reference>();
        for (std::size_t index = 0; index < references.getSize(); ++index)
        {
            const ESM4::Reference* marker = references.at(index);
            if (marker == nullptr || !marker->mIsMapMarker || marker->mFullName.empty()
                || world->getFalloutMapMarkerState(marker->mId) == 0)
                continue;
            mFalloutMapMarkers.emplace(marker->mId, createFalloutMapMarker(*marker));
        }
        Log(Debug::Info) << "FNV/ESM4 map: rendered " << mFalloutMapMarkers.size()
                         << " exact world-map markers";
    }

    void MapWindow::addVisitedLocation(const std::string& name, int x, int y)
    {
        CellId cell;
        cell.first = x;
        cell.second = y;
        if (mMarkers.insert(cell).second)
        {
            MapMarkerType mapMarkerWidget = { osg::Vec2f(x, y), createMarker(name, x, y, 0) };
            mGlobalMapMarkers.emplace(mapMarkerWidget, std::vector<MapMarkerType>());

            const std::string markerName = name.substr(0, name.find(','));
            auto& entry = mGlobalMapMarkersByName[markerName];
            if (!entry.widget)
            {
                entry = { osg::Vec2f(x, y), entry.widget }; // update the coords

                entry.widget = createMarker(markerName, entry.position.x(), entry.position.y(), 1);
                mGlobalMapMarkers.emplace(entry, std::vector<MapMarkerType>{ entry });
            }
            else
            {
                auto it = mGlobalMapMarkers.find(entry);
                auto& marker = const_cast<MapMarkerType&>(it->first);
                auto& elements = it->second;
                elements.emplace_back(mapMarkerWidget);

                // we compute the barycenter of the entry elements => it will be the place on the world map for the
                // agregated widget
                marker.position = std::accumulate(elements.begin(), elements.end(), osg::Vec2f(0.f, 0.f),
                                      [](const auto& left, const auto& right) { return left + right.position; })
                    / float(elements.size());

                marker.widget->setCoord(createMarkerCoords(marker.position.x(), marker.position.y(), elements.size()));
                marker.widget->setVisible(marker.widget->getHeight() >= 6);
            }
        }
    }

    void MapWindow::cellExplored(int x, int y)
    {
        mGlobalMapRender->cleanupCameras();
        mGlobalMapRender->exploreCell(x, y, mLocalMapRender->getMapTexture(x, y));
    }

    void MapWindow::onFrame(float dt)
    {
        LocalMapBase::onFrame(dt);
        NoDrop::onFrame(dt);
    }

    void MapWindow::setGlobalMapMarkerTooltip(MyGUI::Widget* markerWidget, int x, int y)
    {
        ESM::RefId cellRefId = ESM::RefId::esm3ExteriorCell(x, y);
        CustomMarkerCollection::RangeType markers = mCustomMarkers.getMarkers(cellRefId);
        std::vector<std::string> destNotes;
        for (CustomMarkerCollection::ContainerType::const_iterator it = markers.first; it != markers.second; ++it)
            destNotes.push_back(it->second.mNote);

        if (!destNotes.empty())
        {
            MarkerUserData data(nullptr);
            std::swap(data.notes, destNotes);
            data.caption = markerWidget->getUserString("Caption_TextOneLine");
            markerWidget->setUserData(data);
            markerWidget->setUserString("ToolTipType", "MapMarker");
        }
        else
        {
            markerWidget->setUserString("ToolTipType", "Layout");
        }
    }

    float MapWindow::getMarkerSize(size_t agregatedWeight) const
    {
        float markerSize = 12.f * mGlobalMapZoom;
        if (mGlobalMapZoom < 1)
            return markerSize * std::sqrt(agregatedWeight); // we want to see agregated object
        return agregatedWeight ? 0 : markerSize; // we want to see only original markers (i.e. non agregated)
    }

    void MapWindow::resizeGlobalMap()
    {
        mGlobalMap->setCanvasSize(
            mGlobalMapRender->getWidth() * mGlobalMapZoom, mGlobalMapRender->getHeight() * mGlobalMapZoom);
        mGlobalMapImage->setSize(
            mGlobalMapRender->getWidth() * mGlobalMapZoom, mGlobalMapRender->getHeight() * mGlobalMapZoom);
    }

    void MapWindow::worldPosToGlobalMapImageSpace(float x, float y, float& imageX, float& imageY) const
    {
        if (isFalloutContentLoaded())
        {
            const std::optional<FalloutWorldMapGeometry> geometry = getFalloutWorldMapGeometry();
            if (!geometry)
            {
                imageX = 0.f;
                imageY = 0.f;
                return;
            }
            const FalloutMapImagePosition image = projectFalloutWorldMapPosition(x, y, *geometry, mGlobalMapZoom);
            imageX = image.mX;
            imageY = image.mY;

            static int loggedFalloutMapProjection = 0;
            if (loggedFalloutMapProjection < 12)
            {
                Log(Debug::Info) << "FNV/ESM4 proof: Fallout world map projection world=(" << x << "," << y
                                 << ") image=(" << imageX << "," << imageY << ")";
                ++loggedFalloutMapProjection;
            }
            return;
        }

        mGlobalMapRender->worldPosToImageSpace(x, y, imageX, imageY);
        imageX *= mGlobalMapZoom;
        imageY *= mGlobalMapZoom;
    }

    void MapWindow::updateCustomMarkers()
    {
        LocalMapBase::updateCustomMarkers();

        for (auto& [widgetPair, ignore] : mGlobalMapMarkers)
            setGlobalMapMarkerTooltip(widgetPair.widget, widgetPair.position.x(), widgetPair.position.y());
    }

    void MapWindow::onDragStart(MyGUI::Widget* /*sender*/, int left, int top, MyGUI::MouseButton id)
    {
        if (id != MyGUI::MouseButton::Left)
            return;
        mLastDragPos = MyGUI::IntPoint(left, top);
    }

    void MapWindow::onMouseDrag(MyGUI::Widget* /*sender*/, int left, int top, MyGUI::MouseButton id)
    {
        if (id != MyGUI::MouseButton::Left)
            return;

        MyGUI::IntPoint diff = MyGUI::IntPoint(left, top) - mLastDragPos;

        if (!Settings::map().mGlobal)
        {
            mNeedDoorMarkersUpdate = true;
            mLocalMap->setViewOffset(mLocalMap->getViewOffset() + diff);
        }
        else
            mGlobalMap->setViewOffset(mGlobalMap->getViewOffset() + diff);

        mLastDragPos = MyGUI::IntPoint(left, top);
    }

    void MapWindow::onWorldButtonClicked(MyGUI::Widget* /*sender*/)
    {
        const bool global = !Settings::map().mGlobal;

        Settings::map().mGlobal.set(global);

        mGlobalMap->setVisible(global);
        mLocalMap->setVisible(!global);

        bool falloutContent = isFalloutContentLoaded();

        if (falloutContent)
        {
            mButton->setCaption(getFalloutMapToggleLabel(global));
            Log(Debug::Info) << "FNV/ESM4 proof: map toggle label applied " << getFalloutMapModeLabel(global)
                             << " button=" << getFalloutMapToggleLabel(global);
        }
        else
            mButton->setCaptionWithReplacing(global ? "#{sLocal}" : "#{sWorld}");
        mControllerButtons.mX = global ? "#{Interface:Local}" : "#{Interface:World}";
        MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();
    }

    void MapWindow::onPinToggled()
    {
        Settings::windows().mMapPin.set(mPinned);

        MWBase::Environment::get().getWindowManager()->setMinimapVisibility(!mPinned);
    }

    void MapWindow::onTitleDoubleClicked()
    {
        if (Settings::gui().mControllerMenus)
            return;
        else if (MyGUI::InputManager::getInstance().isShiftPressed())
            MWBase::Environment::get().getWindowManager()->toggleMaximized(this);
        else if (!mPinned)
            MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Map);
    }

    void MapWindow::onOpen()
    {
        ensureGlobalMapLoaded();
        if (isFalloutContentLoaded() && Settings::map().mGlobal)
        {
            mLocalMap->setVisible(false);
            mGlobalMapOverlay->setVisible(false);
            mGlobalMapOverlay->setImageTexture({});
        }
        refreshFalloutMapMarkers();

        globalMapUpdatePlayer();
    }

    void MapWindow::fitFalloutWorldMapOnce()
    {
        if (mFalloutInitialMapFitApplied || !isFalloutContentLoaded() || !Settings::map().mGlobal)
            return;
        const std::optional<FalloutWorldMapGeometry> geometry = getFalloutWorldMapGeometry();
        if (!geometry)
            return;
        mGlobalMapZoom = std::min(static_cast<float>(mGlobalMap->getWidth()) / geometry->mWidth,
            static_cast<float>(mGlobalMap->getHeight()) / geometry->mHeight);
        mFalloutInitialMapFitApplied = true;
        updateGlobalMap();
        const MyGUI::IntSize canvas = mGlobalMap->getCanvasSize();
        mGlobalMap->setViewOffset(MyGUI::IntPoint((mGlobalMap->getWidth() - canvas.width) / 2,
            (mGlobalMap->getHeight() - canvas.height) / 2));
        Log(Debug::Info) << "FNV/ESM4 map: fitted authored world map zoom=" << mGlobalMapZoom
                         << " viewport=" << mGlobalMap->getWidth() << "x" << mGlobalMap->getHeight()
                         << " canvas=" << canvas.width << "x" << canvas.height;
    }

    bool MapWindow::focusFalloutMapMarker(ESM::FormId marker, float zoom)
    {
        const auto selected = mFalloutMapMarkers.find(marker);
        const std::optional<FalloutWorldMapGeometry> geometry = getFalloutWorldMapGeometry();
        if (selected == mFalloutMapMarkers.end() || !geometry)
            return false;

        const float fitZoom = std::min(static_cast<float>(mGlobalMap->getWidth()) / geometry->mWidth,
            static_cast<float>(mGlobalMap->getHeight()) / geometry->mHeight);
        mGlobalMapZoom = std::clamp(zoom, fitZoom, 4.f);
        updateGlobalMap();
        for (const auto& [id, widget] : mFalloutMapMarkers)
            widget->setColour(id == marker ? MyGUI::Colour(0.35f, 1.f, 0.35f, 1.f) : MyGUI::Colour::White);

        const MyGUI::IntPoint center
            = selected->second->getPosition()
            + MyGUI::IntPoint(selected->second->getWidth() / 2, selected->second->getHeight() / 2);
        mGlobalMap->setViewOffset(MyGUI::IntPoint(
            mGlobalMap->getWidth() / 2 - center.left, mGlobalMap->getHeight() / 2 - center.top));
        Log(Debug::Info) << "FNV/ESM4 map: focused marker=" << marker << " zoom=" << mGlobalMapZoom
                         << " center=(" << center.left << "," << center.top << ")";
        return true;
    }

    void MapWindow::globalMapUpdatePlayer()
    {
        // For interiors, position is set by WindowManager via setGlobalMapPlayerPosition
        if (MWBase::Environment::get().getWorld()->isCellExterior())
        {
            osg::Vec3f pos = MWBase::Environment::get().getWorld()->getPlayerPtr().getRefData().getPosition().asVec3();
            setGlobalMapPlayerPosition(pos.x(), pos.y());
        }
    }

    void MapWindow::notifyPlayerUpdate()
    {
        globalMapUpdatePlayer();

        setGlobalMapPlayerDir(mLastDirectionX, mLastDirectionY);
    }

    void MapWindow::centerView()
    {
        if (isFalloutContentLoaded() && Settings::map().mGlobal)
        {
            MyGUI::IntSize viewsize = mGlobalMap->getSize();
            MyGUI::IntPoint pos = mPlayerArrowGlobal->getPosition() + MyGUI::IntPoint{ 16, 16 };
            mGlobalMap->setViewOffset(MyGUI::IntPoint(
                static_cast<int>(viewsize.width * 0.5f - pos.left),
                static_cast<int>(viewsize.height * 0.5f - pos.top)));
            return;
        }

        LocalMapBase::centerView();
        // set the view offset so that player is in the center
        MyGUI::IntSize viewsize = mGlobalMap->getSize();
        MyGUI::IntPoint pos = mPlayerArrowGlobal->getPosition() + MyGUI::IntPoint{ 16, 16 };
        MyGUI::IntPoint viewoffs(
            static_cast<int>(viewsize.width * 0.5f - pos.left), static_cast<int>(viewsize.height * 0.5f - pos.top));
        mGlobalMap->setViewOffset(viewoffs);
    }

    void MapWindow::setGlobalMapPlayerPosition(float worldX, float worldY)
    {
        float x, y;
        worldPosToGlobalMapImageSpace(worldX, worldY, x, y);
        mPlayerArrowGlobal->setPosition(MyGUI::IntPoint(static_cast<int>(x - 16), static_cast<int>(y - 16)));
        static bool loggedFalloutGlobalTracking = false;
        if (!loggedFalloutGlobalTracking && isFalloutContentLoaded())
        {
            loggedFalloutGlobalTracking = true;
            Log(Debug::Info) << "FNV/ESM4 proof: global map tracks player world=(" << worldX << "," << worldY
                             << ") image=(" << x << "," << y << ") arrow=(" << mPlayerArrowGlobal->getLeft()
                             << "," << mPlayerArrowGlobal->getTop() << ")";
        }
    }

    void MapWindow::setGlobalMapPlayerDir(const float x, const float y)
    {
        MyGUI::ISubWidget* main = mPlayerArrowGlobal->getSubWidgetMain();
        MyGUI::RotatingSkin* rotatingSubskin = main->castType<MyGUI::RotatingSkin>();
        rotatingSubskin->setCenter(MyGUI::IntPoint(16, 16));
        float angle = std::atan2(x, y);
        rotatingSubskin->setAngle(angle);
    }

    void MapWindow::ensureGlobalMapLoaded()
    {
        if (isFalloutContentLoaded())
        {
            const std::string mapTexture = getFalloutWorldMapTexture();
            if (!mGlobalMapTexture)
            {
                mGlobalMapTexture = std::make_unique<MyGUIPlatform::OSGTexture>(
                    mapTexture, MWBase::Environment::get().getResourceSystem()->getImageManager());
                mGlobalMapTexture->loadFromFile(mapTexture);
            }
            mGlobalMapImage->setRenderItemTexture(mGlobalMapTexture.get());
            mGlobalMapImage->setImageTexture(mapTexture);
            mGlobalMapImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 0.f, 1.f, 1.f));
            const std::optional<FalloutWorldMapGeometry> geometry = getFalloutWorldMapGeometry();
            if (!geometry)
                throw std::runtime_error("Fallout WastelandNV has no authored MNAM world-map geometry");
            const int mapWidth = static_cast<int>(geometry->mWidth);
            const int mapHeight = static_cast<int>(geometry->mHeight);
            mGlobalMapImage->setImageCoord(MyGUI::IntCoord(0, 0, mapWidth, mapHeight));
            mGlobalMapOverlay->setVisible(false);
            mGlobalMapOverlay->setImageTexture({});
            mGlobalMapOverlay->setImageCoord(MyGUI::IntCoord(0, 0, mapWidth, mapHeight));
            mGlobalMapImage->setSize(
                static_cast<int>(geometry->mWidth * mGlobalMapZoom),
                static_cast<int>(geometry->mHeight * mGlobalMapZoom));
            mGlobalMapOverlay->setSize(mGlobalMapImage->getSize());
            mGlobalMap->setCanvasSize(mGlobalMapImage->getSize());
            mGlobalMap->getParent()->_updateChilds();
            Log(Debug::Info) << "FNV/ESM4 proof: Fallout world map texture refreshed " << mapTexture
                             << " zoom=" << mGlobalMapZoom;
            return;
        }

        if (!mGlobalMapTexture.get())
        {
            mGlobalMapTexture = std::make_unique<MyGUIPlatform::OSGTexture>(mGlobalMapRender->getBaseTexture());
            mGlobalMapImage->setRenderItemTexture(mGlobalMapTexture.get());
            mGlobalMapImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 0.f, 1.f, 1.f));

            mGlobalMapOverlayTexture
                = std::make_unique<MyGUIPlatform::OSGTexture>(mGlobalMapRender->getOverlayTexture());
            mGlobalMapOverlay->setRenderItemTexture(mGlobalMapOverlayTexture.get());
            mGlobalMapOverlay->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 0.f, 1.f, 1.f));

            // Redraw children in proper order
            mGlobalMap->getParent()->_updateChilds();
        }
    }

    void MapWindow::clear()
    {
        mMarkers.clear();

        mGlobalMapRender->clear();
        mActiveCell = nullptr;

        for (auto& widgetPair : mGlobalMapMarkers)
            MyGUI::Gui::getInstance().destroyWidget(widgetPair.first.widget);
        mGlobalMapMarkers.clear();
        mGlobalMapMarkersByName.clear();
        for (const auto& [id, widget] : mFalloutMapMarkers)
            MyGUI::Gui::getInstance().destroyWidget(widget);
        mFalloutMapMarkers.clear();
    }

    void MapWindow::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        ESM::GlobalMap map;
        mGlobalMapRender->write(map);

        map.mMarkers = mMarkers;

        writer.startRecord(ESM::REC_GMAP);
        map.save(writer);
        writer.endRecord(ESM::REC_GMAP);
    }

    void MapWindow::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_GMAP)
        {
            ESM::GlobalMap map;
            map.load(reader);

            mGlobalMapRender->read(map);

            for (const ESM::GlobalMap::CellId& cellId : map.mMarkers)
            {
                const ESM::Cell* cell
                    = MWBase::Environment::get().getESMStore()->get<ESM::Cell>().search(cellId.first, cellId.second);
                if (cell && !cell->mName.empty())
                    addVisitedLocation(cell->mName, cellId.first, cellId.second);
            }
        }
    }

    void MapWindow::setAlpha(float alpha)
    {
        NoDrop::setAlpha(alpha);
        // can't allow showing map with partial transparency, as the fog of war will also go transparent
        // and reveal parts of the map you shouldn't be able to see
        for (MapEntry& entry : mMaps)
            entry.mMapWidget->setVisible(alpha == 1);
    }

    void MapWindow::customMarkerCreated(MyGUI::Widget* marker)
    {
        marker->eventMouseDrag += MyGUI::newDelegate(this, &MapWindow::onMouseDrag);
        marker->eventMouseButtonPressed += MyGUI::newDelegate(this, &MapWindow::onDragStart);
        marker->eventMouseButtonDoubleClick += MyGUI::newDelegate(this, &MapWindow::onCustomMarkerDoubleClicked);
        if (Settings::map().mAllowZooming)
            marker->eventMouseWheel += MyGUI::newDelegate(this, &MapWindow::onMapZoomed);
    }

    void MapWindow::doorMarkerCreated(MyGUI::Widget* marker)
    {
        marker->eventMouseDrag += MyGUI::newDelegate(this, &MapWindow::onMouseDrag);
        marker->eventMouseButtonPressed += MyGUI::newDelegate(this, &MapWindow::onDragStart);
        if (Settings::map().mAllowZooming)
            marker->eventMouseWheel += MyGUI::newDelegate(this, &MapWindow::onMapZoomed);
    }

    void MapWindow::asyncPrepareSaveMap()
    {
        mGlobalMapRender->asyncWritePng();
    }

    bool MapWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
            MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            onWorldButtonClicked(mButton);
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_Y)
        {
            centerView();
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
            shiftMap(0, 100);
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            shiftMap(0, -100);
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
            shiftMap(100, 0);
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
            shiftMap(-100, 0);

        return true;
    }

    void MapWindow::shiftMap(int dx, int dy)
    {
        if (dx == 0 && dy == 0)
            return;

        if (!Settings::map().mGlobal)
        {
            mNeedDoorMarkersUpdate = true;
            mLocalMap->setViewOffset(
                MyGUI::IntPoint(mLocalMap->getViewOffset().left + dx, mLocalMap->getViewOffset().top + dy));
        }
        else
        {
            mGlobalMap->setViewOffset(
                MyGUI::IntPoint(mGlobalMap->getViewOffset().left + dx, mGlobalMap->getViewOffset().top + dy));
        }
    }

    void MapWindow::setActiveControllerWindow(bool active)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->getMode() == MWGui::GM_Inventory)
        {
            // Fill the screen, or limit to a certain size on large screens. Size chosen to
            // show the entire local map without scrolling.
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            MyGUI::IntSize canvasSize = mLocalMap->getCanvasSize();
            MyGUI::IntSize borderSize = mMainWidget->getSize() - mMainWidget->getClientWidget()->getSize();

            int width = std::min(viewSize.width, canvasSize.width + borderSize.width);
            int height = std::min(winMgr->getControllerMenuHeight(), canvasSize.height + borderSize.height);
            int x = (viewSize.width - width) / 2;
            int y = (viewSize.height - height) / 2;

            MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
            window->setCoord(x, active ? y : viewSize.height + 1, width, height);
        }

        WindowBase::setActiveControllerWindow(active);
    }

    // -------------------------------------------------------------------

    EditNoteDialog::EditNoteDialog()
        : WindowModal("openmw_edit_note.layout")
    {
        getWidget(mOkButton, "OkButton");
        getWidget(mCancelButton, "CancelButton");
        getWidget(mDeleteButton, "DeleteButton");
        getWidget(mTextEdit, "TextEdit");

        mCancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EditNoteDialog::onCancelButtonClicked);
        mOkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EditNoteDialog::onOkButtonClicked);
        mDeleteButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EditNoteDialog::onDeleteButtonClicked);

        if (Settings::gui().mControllerMenus)
        {
            mControllerButtons.mA = "#{Interface:OK}";
            mControllerButtons.mB = "#{Interface:Cancel}";
        }
    }

    void EditNoteDialog::showDeleteButton(bool show)
    {
        mDeleteButton->setVisible(show);
    }

    bool EditNoteDialog::getDeleteButtonShown()
    {
        return mDeleteButton->getVisible();
    }

    void EditNoteDialog::setText(const std::string& text)
    {
        mTextEdit->setCaption(MyGUI::TextIterator::toTagsString(text));
    }

    std::string EditNoteDialog::getText()
    {
        return MyGUI::TextIterator::getOnlyText(mTextEdit->getCaption());
    }

    void EditNoteDialog::onOpen()
    {
        WindowModal::onOpen();
        center();
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mTextEdit);

        if (Settings::gui().mControllerMenus)
        {
            mControllerFocus = getDeleteButtonShown() ? 1 : 0;
            mOkButton->setStateSelected(true);
            mCancelButton->setStateSelected(false);
        }
    }

    void EditNoteDialog::onCancelButtonClicked(MyGUI::Widget* /*sender*/)
    {
        setVisible(false);
    }

    void EditNoteDialog::onOkButtonClicked(MyGUI::Widget* /*sender*/)
    {
        eventOkClicked();
    }

    void EditNoteDialog::onDeleteButtonClicked(MyGUI::Widget* /*sender*/)
    {
        eventDeleteClicked();
    }

    ControllerButtons* EditNoteDialog::getControllerButtons()
    {
        if (getDeleteButtonShown())
            mControllerButtons.mX = "#{Interface:Delete}";
        else
            mControllerButtons.mX.clear();
        return &mControllerButtons;
    }

    bool EditNoteDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (getDeleteButtonShown())
            {
                if (mControllerFocus == 0)
                    onDeleteButtonClicked(mDeleteButton);
                else if (mControllerFocus == 1)
                    onOkButtonClicked(mOkButton);
                else
                    onCancelButtonClicked(mCancelButton);
            }
            else
            {
                if (mControllerFocus == 0)
                    onOkButtonClicked(mOkButton);
                else
                    onCancelButtonClicked(mCancelButton);
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onCancelButtonClicked(mCancelButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            if (getDeleteButtonShown())
                onDeleteButtonClicked(mDeleteButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        {
            if (getDeleteButtonShown())
            {
                mControllerFocus = wrap(mControllerFocus - 1, 3);
                mDeleteButton->setStateSelected(mControllerFocus == 0);
                mOkButton->setStateSelected(mControllerFocus == 1);
                mCancelButton->setStateSelected(mControllerFocus == 2);
            }
            else
            {
                mControllerFocus = 0;
                mOkButton->setStateSelected(mControllerFocus == 0);
                mCancelButton->setStateSelected(mControllerFocus == 1);
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            if (getDeleteButtonShown())
            {
                mControllerFocus = wrap(mControllerFocus + 1, 3);
                mDeleteButton->setStateSelected(mControllerFocus == 0);
                mOkButton->setStateSelected(mControllerFocus == 1);
                mCancelButton->setStateSelected(mControllerFocus == 2);
            }
            else
            {
                mControllerFocus = 1;
                mOkButton->setStateSelected(mControllerFocus == 0);
                mCancelButton->setStateSelected(mControllerFocus == 1);
            }
        }

        return true;
    }

    bool LocalMapBase::MarkerUserData::isPositionExplored() const
    {
        if (!mLocalMapRender)
            return true;
        return mLocalMapRender->isPositionExplored(nX, nY, cellX, cellY);
    }

}
