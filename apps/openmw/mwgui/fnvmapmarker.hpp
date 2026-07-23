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

    inline FalloutMapImagePosition projectFalloutWorldMapPosition(float worldX, float worldY, float zoom = 1.f)
    {
        constexpr float minCellX = -20.f;
        constexpr float maxCellX = 16.f;
        constexpr float minCellY = -16.f;
        constexpr float maxCellY = 16.f;
        constexpr float mapSize = 1024.f;
        const float cellX = worldX / static_cast<float>(Constants::CellSizeInUnits);
        const float cellY = worldY / static_cast<float>(Constants::CellSizeInUnits);
        return {
            std::clamp((cellX - minCellX) / (maxCellX - minCellX), 0.f, 1.f) * mapSize * zoom,
            std::clamp((maxCellY - cellY) / (maxCellY - minCellY), 0.f, 1.f) * mapSize * zoom,
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
