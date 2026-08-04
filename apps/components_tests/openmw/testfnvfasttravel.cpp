#include "../../openmw/mwworld/fnvfasttravel.hpp"

#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadwrld.hpp>

#include <gtest/gtest.h>

namespace
{
    ESM::FormId formId(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    ESM::RefId refId(std::uint32_t value)
    {
        return ESM::RefId::formIdRefId(formId(value));
    }

    ESM4::World makeWorld(std::uint32_t value, std::uint8_t flags = 0)
    {
        ESM4::World world;
        world.mId = formId(value);
        world.mWorldFlags = flags;
        return world;
    }

    ESM4::Cell makeExteriorCell(std::uint32_t value, std::uint32_t worldspace)
    {
        ESM4::Cell cell;
        cell.mId = refId(value);
        cell.mParent = refId(worldspace);
        cell.mCellFlags = 0;
        cell.mX = -3;
        cell.mY = 7;
        return cell;
    }

    ESM4::Cell makeInteriorCell(std::uint32_t value, std::uint32_t worldspace)
    {
        ESM4::Cell cell = makeExteriorCell(value, worldspace);
        cell.mCellFlags = ESM4::CELL_Interior;
        return cell;
    }

    ESM4::Reference makeMarker(std::uint32_t value, std::uint32_t cell, bool isMarker = true)
    {
        ESM4::Reference marker;
        marker.mId = formId(value);
        marker.mParent = refId(cell);
        marker.mIsMapMarker = isMarker;
        marker.mFullName = isMarker ? "Test destination" : "Not a marker";
        marker.mPos.pos[0] = 123.f;
        marker.mPos.pos[1] = 456.f;
        marker.mPos.pos[2] = 789.f;
        return marker;
    }

    MWWorld::FalloutFastTravelResolution resolve(const ESM4::Reference* marker,
        const ESM4::Cell* destinationCell, const ESM4::World* destinationWorld, std::uint8_t markerState,
        const ESM4::Cell* currentCell, const ESM4::World* currentWorld, bool scriptedFastTravelEnabled = true,
        bool enemiesNearby = false)
    {
        return MWWorld::resolveFalloutFastTravelDestination(marker, destinationCell, destinationWorld, markerState,
            currentCell, currentWorld, scriptedFastTravelEnabled, enemiesNearby);
    }
}

TEST(FNVFastTravel, ResolvesDiscoveredExteriorDestinationAndSameCell)
{
    constexpr std::uint32_t worldspaceId = 0x01000001;
    constexpr std::uint32_t cellId = 0x01000002;
    ESM4::World world = makeWorld(worldspaceId);
    ESM4::Cell cell = makeExteriorCell(cellId, worldspaceId);
    ESM4::Reference marker = makeMarker(0x01000003, cellId);

    const MWWorld::FalloutFastTravelResolution result
        = resolve(&marker, &cell, &world, 2, &cell, &world);
    ASSERT_TRUE(result) << result.mError;
    ASSERT_TRUE(result.mDestination.has_value());
    EXPECT_EQ(result.mDestination->mCell, marker.mParent);
    EXPECT_FLOAT_EQ(result.mDestination->mPosition.pos[0], 123.f);
    EXPECT_FLOAT_EQ(result.mDestination->mPosition.pos[1], 456.f);
    EXPECT_FLOAT_EQ(result.mDestination->mPosition.pos[2], 789.f);
}

TEST(FNVFastTravel, RejectsHiddenAndVisibleOnlyMarkers)
{
    ESM4::World world = makeWorld(0x01000010);
    ESM4::Cell cell = makeExteriorCell(0x01000011, 0x01000010);
    ESM4::Reference marker = makeMarker(0x01000012, 0x01000011);

    const auto hidden = resolve(&marker, &cell, &world, 0, &cell, &world);
    EXPECT_FALSE(hidden);
    EXPECT_EQ(hidden.mError, "You have not discovered that location.");

    const auto visibleOnly = resolve(&marker, &cell, &world, 1, &cell, &world);
    EXPECT_FALSE(visibleOnly);
    EXPECT_EQ(visibleOnly.mError, "You have not discovered that location.");
}

TEST(FNVFastTravel, RejectsInvalidMarkerAndMissingDestination)
{
    ESM4::World world = makeWorld(0x01000020);
    ESM4::Cell cell = makeExteriorCell(0x01000021, 0x01000020);
    ESM4::Reference marker = makeMarker(0x01000022, 0x01000021);
    ESM4::Reference nonMarker = makeMarker(0x01000023, 0x01000021, false);

    const auto invalidForm = resolve(nullptr, &cell, &world, 2, &cell, &world);
    EXPECT_FALSE(invalidForm);
    EXPECT_EQ(invalidForm.mError, "That location is not a valid map marker.");

    const auto nonMarkerResult = resolve(&nonMarker, &cell, &world, 2, &cell, &world);
    EXPECT_FALSE(nonMarkerResult);
    EXPECT_EQ(nonMarkerResult.mError, "That location is not a valid map marker.");

    const auto nullCell = resolve(&marker, nullptr, &world, 2, &cell, &world);
    EXPECT_FALSE(nullCell);
    EXPECT_EQ(nullCell.mError, "The map marker has no authored exterior destination.");

    ESM4::Cell wrongCell = makeExteriorCell(0x01000024, 0x01000020);
    const auto wrongCellId = resolve(&marker, &wrongCell, &world, 2, &cell, &world);
    EXPECT_FALSE(wrongCellId);
    EXPECT_EQ(wrongCellId.mError, "The map marker has no authored exterior destination.");
}

TEST(FNVFastTravel, RejectsInteriorAndWorldspaceMismatch)
{
    ESM4::World world = makeWorld(0x01000030);
    ESM4::Cell interior = makeInteriorCell(0x01000031, 0x01000030);
    ESM4::Reference marker = makeMarker(0x01000032, 0x01000031);
    const auto interiorResult = resolve(&marker, &interior, &world, 2, nullptr, nullptr);
    EXPECT_FALSE(interiorResult);
    EXPECT_EQ(interiorResult.mError, "The map marker has no authored exterior destination.");

    ESM4::Cell exterior = makeExteriorCell(0x01000033, 0x01000030);
    ESM4::World wrongWorld = makeWorld(0x01000034);
    marker.mParent = exterior.mId;
    const auto wrongWorldResult = resolve(&marker, &exterior, &wrongWorld, 2, nullptr, nullptr);
    EXPECT_FALSE(wrongWorldResult);
    EXPECT_EQ(wrongWorldResult.mError, "The map marker destination has no authored worldspace.");
}

TEST(FNVFastTravel, RejectsDisabledGlobalTravelAndNearbyEnemies)
{
    ESM4::World world = makeWorld(0x01000040);
    ESM4::Cell cell = makeExteriorCell(0x01000041, 0x01000040);
    ESM4::Reference marker = makeMarker(0x01000042, 0x01000041);

    const auto disabled = resolve(&marker, &cell, &world, 2, &cell, &world, false, false);
    EXPECT_FALSE(disabled);
    EXPECT_EQ(disabled.mError, "Fast travel is currently unavailable from this location.");

    const auto enemies = resolve(&marker, &cell, &world, 2, &cell, &world, true, true);
    EXPECT_FALSE(enemies);
    EXPECT_EQ(enemies.mError, "You cannot fast travel when enemies are nearby.");
}

TEST(FNVFastTravel, RejectsNoTravelCurrentCellAndWorld)
{
    ESM4::World world = makeWorld(0x01000050);
    ESM4::Cell destination = makeExteriorCell(0x01000051, 0x01000050);
    ESM4::Reference marker = makeMarker(0x01000052, 0x01000051);
    ESM4::Cell current = makeInteriorCell(0x01000053, 0x01000050);
    current.mCellFlags |= ESM4::CELL_NoTravel;

    const auto noTravelCell = resolve(&marker, &destination, &world, 2, &current, &world);
    EXPECT_FALSE(noTravelCell);
    EXPECT_EQ(noTravelCell.mError, "You cannot fast travel from this location.");

    ESM4::World noTravelWorld = makeWorld(0x01000054, ESM4::World::WLD_NoFastTravel);
    destination.mParent = refId(0x01000054);
    marker.mParent = destination.mId;
    const auto noTravelWorldResult = resolve(&marker, &destination, &noTravelWorld, 2, nullptr, nullptr);
    EXPECT_FALSE(noTravelWorldResult);
    EXPECT_EQ(noTravelWorldResult.mError, "You cannot fast travel to that worldspace.");

    ESM4::World currentNoTravelWorld = makeWorld(0x01000055, ESM4::World::WLD_NoFastTravel);
    destination.mParent = refId(0x01000050);
    marker.mParent = destination.mId;
    const auto currentWorldResult = resolve(&marker, &destination, &world, 2, nullptr, &currentNoTravelWorld);
    EXPECT_FALSE(currentWorldResult);
    EXPECT_EQ(currentWorldResult.mError, "You cannot fast travel from this worldspace.");
}
