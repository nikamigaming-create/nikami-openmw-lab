#ifndef OPENMW_MWGUI_FNVMAPMARKER_H
#define OPENMW_MWGUI_FNVMAPMARKER_H

#include <algorithm>
#include <cstdint>
#include <string_view>

#include <components/esm4/loadrefr.hpp>
#include <components/misc/constants.hpp>

namespace MWGui
{
    struct FalloutMapImagePosition
    {
        float mX = 0.f;
        float mY = 0.f;

        friend bool operator==(const FalloutMapImagePosition&, const FalloutMapImagePosition&) = default;
    };

    struct FalloutWorldMapGeometry
    {
        float mNorthWestCellX = 0.f;
        float mNorthWestCellY = 0.f;
        float mSouthEastCellX = 0.f;
        float mSouthEastCellY = 0.f;
        float mWidth = 0.f;
        float mHeight = 0.f;
    };

    inline FalloutMapImagePosition projectFalloutWorldMapPosition(
        float worldX, float worldY, const FalloutWorldMapGeometry& map, float zoom = 1.f)
    {
        const float cellX = worldX / static_cast<float>(Constants::CellSizeInUnits);
        const float cellY = worldY / static_cast<float>(Constants::CellSizeInUnits);
        const float cellWidth = map.mSouthEastCellX - map.mNorthWestCellX;
        const float cellHeight = map.mNorthWestCellY - map.mSouthEastCellY;
        if (cellWidth <= 0.f || cellHeight <= 0.f || map.mWidth <= 0.f || map.mHeight <= 0.f)
            return {};
        return {
            std::clamp((cellX - map.mNorthWestCellX) / cellWidth, 0.f, 1.f) * map.mWidth * zoom,
            std::clamp((map.mNorthWestCellY - cellY) / cellHeight, 0.f, 1.f) * map.mHeight * zoom,
        };
    }

    inline std::string_view getFalloutMapMarkerIcon(std::uint8_t type)
    {
        switch (type)
        {
            case ESM4::Map_City:
                return "textures\\interface\\icons\\world map\\icon_map_city.dds";
            case ESM4::Map_Settlement:
                return "textures\\interface\\icons\\world map\\icon_map_settlement.dds";
            case ESM4::Map_Encampment:
                return "textures\\interface\\icons\\world map\\icon_map_encampment.dds";
            case ESM4::Map_NaturalLandmark:
                return "textures\\interface\\icons\\world map\\icon_map_natural_landmark.dds";
            case ESM4::Map_Cave:
                return "textures\\interface\\icons\\world map\\icon_map_cave.dds";
            case ESM4::Map_Factory:
                return "textures\\interface\\icons\\world map\\icon_map_factory.dds";
            case ESM4::Map_Monument:
                return "textures\\interface\\icons\\world map\\icon_map_monument.dds";
            case ESM4::Map_Military:
                return "textures\\interface\\icons\\world map\\icon_map_military.dds";
            case ESM4::Map_Office:
                return "textures\\interface\\icons\\world map\\icon_map_office.dds";
            case ESM4::Map_TownRuins:
                return "textures\\interface\\icons\\world map\\icon_map_ruins_town.dds";
            case ESM4::Map_UrbanRuins:
                return "textures\\interface\\icons\\world map\\icon_map_ruins_urban.dds";
            case ESM4::Map_SewerRuins:
                return "textures\\interface\\icons\\world map\\icon_map_ruins_sewer.dds";
            case ESM4::Map_Metro:
                return "textures\\interface\\icons\\world map\\icon_map_metro.dds";
            case ESM4::Map_Vault:
                return "textures\\interface\\icons\\world map\\icon_map_vault.dds";
            default:
                return "textures\\interface\\icons\\world map\\icon_map_undiscovered.dds";
        }
    }
}

#endif
