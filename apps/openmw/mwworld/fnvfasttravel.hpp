#ifndef OPENMW_MWWORLD_FNVFASTTRAVEL_H
#define OPENMW_MWWORLD_FNVFASTTRAVEL_H

#include <cstdint>
#include <optional>
#include <string>

#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>

namespace ESM4
{
    struct Cell;
    struct Reference;
    struct World;
}

namespace MWWorld
{
    struct FalloutFastTravelDestination
    {
        ESM::RefId mCell;
        ESM::Position mPosition;
    };

    struct FalloutFastTravelResolution
    {
        std::optional<FalloutFastTravelDestination> mDestination;
        std::string mError;

        explicit operator bool() const { return mDestination.has_value(); }
    };

    FalloutFastTravelResolution resolveFalloutFastTravelDestination(const ESM4::Reference* marker,
        const ESM4::Cell* destinationCell, const ESM4::World* destinationWorld, std::uint8_t markerState,
        const ESM4::Cell* currentCell, const ESM4::World* currentWorld, bool enemiesNearby);
}

#endif
