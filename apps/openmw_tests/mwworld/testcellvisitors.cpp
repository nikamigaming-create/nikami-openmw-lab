#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <vector>

#include <components/esm3/cellref.hpp>
#include <components/esm4/loadrefr.hpp>

#include "apps/openmw/mwworld/cellref.hpp"
#include "apps/openmw/mwworld/cellvisitors.hpp"

namespace MWWorld
{
    namespace
    {
        using testing::ElementsAre;

        struct TestDoor
        {
            CellRef mRef;
        };

        TestDoor makeEsm3Door(ESM::FormId id, bool teleport)
        {
            ESM::CellRef ref;
            ref.blank();
            ref.mRefNum = id;
            ref.mTeleport = teleport;
            return TestDoor{ CellRef(ref) };
        }

        TestDoor makeEsm4Door(ESM::FormId id, bool teleport)
        {
            ESM4::Reference ref{};
            ref.mId = id;
            if (teleport)
                ref.mDoor.destDoor = ESM::FormId{ .mIndex = 0x42, .mContentFile = 0 };
            return TestDoor{ CellRef(ref) };
        }

        TEST(MWWorldCellVisitorsTest, findsTeleportDoorsInEsm3AndEsm4Ranges)
        {
            const ESM::FormId esm3TeleportId{ .mIndex = 1, .mContentFile = 0 };
            const ESM::FormId esm4TeleportId{ .mIndex = 2, .mContentFile = 0 };
            const std::array esm3Doors{
                makeEsm3Door(esm3TeleportId, true),
                makeEsm3Door(ESM::FormId{ .mIndex = 3, .mContentFile = 0 }, false),
            };
            const std::array esm4Doors{
                makeEsm4Door(esm4TeleportId, true),
                makeEsm4Door(ESM::FormId{ .mIndex = 4, .mContentFile = 0 }, false),
            };

            std::vector<ESM::RefNum> teleportDoorIds;
            const auto collectDoor = [&](const auto& door) { teleportDoorIds.push_back(door.mRef.getRefNum()); };

            forEachTeleportDoor(esm3Doors, collectDoor);
            forEachTeleportDoor(esm4Doors, collectDoor);

            EXPECT_THAT(teleportDoorIds, ElementsAre(esm3TeleportId, esm4TeleportId));
        }
    }
}
