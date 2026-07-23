#include <gtest/gtest.h>

#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadwrld.hpp>

#include "apps/openmw/mwworld/fnvfasttravel.hpp"

namespace
{
    struct Fixture
    {
        ESM4::Reference mMarker;
        ESM4::Cell mDestinationCell;
        ESM4::World mDestinationWorld;
        ESM4::Cell mCurrentCell;
        ESM4::World mCurrentWorld;

        Fixture()
        {
            mMarker.mId = ESM::FormId{ 0x1234, 1 };
            mMarker.mParent = ESM::RefId(ESM::FormId{ 0x2345, 1 });
            mMarker.mFullName = "Goodsprings";
            mMarker.mIsMapMarker = true;
            mMarker.mPos.pos[0] = -67735.f;
            mMarker.mPos.pos[1] = 3204.f;
            mMarker.mPos.pos[2] = 8425.f;

            mDestinationCell.mId = mMarker.mParent;
            mDestinationWorld.mId = ESM::FormId{ 0x3456, 1 };
            mDestinationWorld.mWorldFlags = 0;
            mDestinationCell.mParent = ESM::RefId(mDestinationWorld.mId);

            mCurrentCell.mId = ESM::RefId(ESM::FormId{ 0x4567, 1 });
            mCurrentWorld.mId = mDestinationWorld.mId;
            mCurrentWorld.mWorldFlags = 0;
            mCurrentCell.mParent = ESM::RefId(mCurrentWorld.mId);
        }

        MWWorld::FalloutFastTravelResolution resolve(std::uint8_t state = 2, bool enemies = false) const
        {
            return MWWorld::resolveFalloutFastTravelDestination(&mMarker, &mDestinationCell, &mDestinationWorld,
                state, &mCurrentCell, &mCurrentWorld, enemies);
        }
    };

    TEST(FalloutFastTravel, ResolvesExactAuthoredExteriorMarkerDestination)
    {
        Fixture fixture;
        const MWWorld::FalloutFastTravelResolution resolution = fixture.resolve();
        ASSERT_TRUE(resolution) << resolution.mError;
        EXPECT_EQ(resolution.mDestination->mCell, fixture.mMarker.mParent);
        EXPECT_FLOAT_EQ(resolution.mDestination->mPosition.pos[0], -67735.f);
        EXPECT_FLOAT_EQ(resolution.mDestination->mPosition.pos[1], 3204.f);
        EXPECT_FLOAT_EQ(resolution.mDestination->mPosition.pos[2], 8425.f);
    }

    TEST(FalloutFastTravel, RejectsUndiscoveredAndVisibleOnlyMarkers)
    {
        Fixture fixture;
        EXPECT_FALSE(fixture.resolve(0));
        EXPECT_FALSE(fixture.resolve(1));
        EXPECT_TRUE(fixture.resolve(2));
    }

    TEST(FalloutFastTravel, RejectsCombatAndNoTravelWorlds)
    {
        Fixture fixture;
        EXPECT_FALSE(fixture.resolve(2, true));

        fixture.mCurrentCell.mCellFlags = ESM4::CELL_NoTravel;
        EXPECT_FALSE(fixture.resolve());
        fixture.mCurrentCell.mCellFlags = 0;

        fixture.mCurrentWorld.mWorldFlags = ESM4::World::WLD_NoFastTravel;
        EXPECT_FALSE(fixture.resolve());
        fixture.mCurrentWorld.mWorldFlags = 0;

        fixture.mDestinationWorld.mWorldFlags = ESM4::World::WLD_NoFastTravel;
        EXPECT_FALSE(fixture.resolve());
    }

    TEST(FalloutFastTravel, RejectsMissingOrMismatchedAuthoredDestination)
    {
        Fixture fixture;
        EXPECT_FALSE(MWWorld::resolveFalloutFastTravelDestination(&fixture.mMarker, nullptr,
            &fixture.mDestinationWorld, 2, &fixture.mCurrentCell, &fixture.mCurrentWorld, false));

        fixture.mDestinationCell.mId = ESM::RefId(ESM::FormId{ 0x9999, 1 });
        EXPECT_FALSE(fixture.resolve());
    }
}
