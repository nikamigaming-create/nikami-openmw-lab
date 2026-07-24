#include "fnvfasttravel.hpp"

#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadwrld.hpp>

namespace MWWorld
{
    FalloutFastTravelResolution resolveFalloutFastTravelDestination(const ESM4::Reference* marker,
        const ESM4::Cell* destinationCell, const ESM4::World* destinationWorld, std::uint8_t markerState,
        const ESM4::Cell* currentCell, const ESM4::World* currentWorld, bool scriptedFastTravelEnabled,
        bool enemiesNearby)
    {
        if (marker == nullptr || !marker->mIsMapMarker || marker->mFullName.empty())
            return { std::nullopt, "That location is not a valid map marker." };
        if (markerState != 2)
            return { std::nullopt, "You have not discovered that location." };
        if (!scriptedFastTravelEnabled)
            return { std::nullopt, "Fast travel is currently unavailable from this location." };
        if (enemiesNearby)
            return { std::nullopt, "You cannot fast travel when enemies are nearby." };
        if (currentCell != nullptr && (currentCell->mCellFlags & ESM4::CELL_NoTravel) != 0)
            return { std::nullopt, "You cannot fast travel from this location." };
        if (currentWorld != nullptr && (currentWorld->mWorldFlags & ESM4::World::WLD_NoFastTravel) != 0)
            return { std::nullopt, "You cannot fast travel from this worldspace." };
        if (destinationCell == nullptr || !destinationCell->isExterior() || destinationCell->mId != marker->mParent)
            return { std::nullopt, "The map marker has no authored exterior destination." };
        if (destinationWorld == nullptr || destinationCell->mParent != destinationWorld->mId)
            return { std::nullopt, "The map marker destination has no authored worldspace." };
        if ((destinationWorld->mWorldFlags & ESM4::World::WLD_NoFastTravel) != 0)
            return { std::nullopt, "You cannot fast travel to that worldspace." };

        return { FalloutFastTravelDestination{ marker->mParent, marker->mPos }, {} };
    }
}
